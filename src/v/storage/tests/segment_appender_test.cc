#include "bytes/iostream.h"
#include "storage/segment_appender.h"
#include "test_utils/random_bytes.h"
#include "test_utils/test.h"

#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/later.hh>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <random>
#include <span>
#include <vector>

static ss::logger tst_log("test-logger");

/// Checks the invariants the appender must uphold at the file boundary, the
/// central one being that the buffer handed to file::dma_write() must not be
/// mutated while that write is in flight.
///
/// Every write_dma() snapshots the caller's buffer and stays "in flight" for a
/// configurable number of reactor turns, or until the test releases it by
/// index. On release the live buffer is compared against the snapshot; a
/// difference means an append mutated a region the DMA was still reading. The
/// underlying write is then issued from the snapshot, modelling a DMA that
/// sampled the buffer when the write was issued -- a correct appender must
/// produce the expected file contents under that model.
struct inflight_write_probe {
    static constexpr size_t dma_alignment = 4_KiB;

    struct entry {
        size_t index{};
        uint64_t pos{};
        const char* buf{};
        size_t len{};
        ss::temporary_buffer<char> snapshot;
        ss::promise<> gate;
        bool released{false};
    };

    // while set, every write parks until the test releases it by index
    bool hold{true};
    // when not holding, how many reactor turns a write stays in flight
    std::function<size_t()> delay_turns = [] { return 1; };

    size_t started{0};
    std::vector<ss::sstring> failures;
    std::vector<ss::lw_shared_ptr<entry>> live;

    void record(ss::sstring msg) {
        vlog(tst_log.error, "{}", msg);
        failures.push_back(std::move(msg));
    }

    /// O_DIRECT requires the file offset, the length and the buffer address to
    /// all be alignment multiples.
    void check_alignment(const entry& e) {
        if (
          e.pos % dma_alignment != 0 || e.len % dma_alignment != 0
          || reinterpret_cast<uintptr_t>(e.buf) % dma_alignment != 0) {
            record(
              fmt::format(
                "dma_write#{} is not {}-aligned: file_pos={} len={} buf={}",
                e.index,
                dma_alignment,
                e.pos,
                e.len,
                fmt::ptr(e.buf)));
        }
    }

    /// Writes to one head chunk are serialised by _prev_head_write, so two
    /// writes must never be in flight over the same file range (the on-disk
    /// result would depend on completion order) nor over the same memory range
    /// (two DMAs reading one chunk region).
    void check_no_concurrent_overlap(const entry& e) {
        for (const auto& o : live) {
            const bool file_overlap = e.pos < o->pos + o->len
                                      && o->pos < e.pos + e.len;
            const bool mem_overlap = e.buf < o->buf + o->len
                                     && o->buf < e.buf + e.len;
            if (file_overlap || mem_overlap) {
                record(
                  fmt::format(
                    "dma_write#{} (file_pos={} len={} buf={}) overlaps "
                    "in-flight "
                    "dma_write#{} (file_pos={} len={} buf={}): file={} mem={}",
                    e.index,
                    e.pos,
                    e.len,
                    fmt::ptr(e.buf),
                    o->index,
                    o->pos,
                    o->len,
                    fmt::ptr(o->buf),
                    file_overlap,
                    mem_overlap));
            }
        }
    }

    void check_not_mutated(const entry& e) {
        const std::span live_bytes(e.buf, e.len);
        const std::span snapshot_bytes(e.snapshot.get(), e.len);
        const auto [it, _] = std::ranges::mismatch(live_bytes, snapshot_bytes);
        if (it == live_bytes.end()) {
            return;
        }
        const auto off = static_cast<size_t>(
          std::ranges::distance(live_bytes.begin(), it));
        record(
          fmt::format(
            "dma_write#{} (file_pos={} len={}) buffer mutated while in flight, "
            "first differing byte at buffer offset {} (file offset {})",
            e.index,
            e.pos,
            e.len,
            off,
            e.pos + off));
    }

    void release(size_t index) {
        auto it = std::ranges::find_if(
          live, [index](const auto& e) { return e->index == index; });
        vassert(it != live.end(), "no in-flight write with index {}", index);
        vassert(!(*it)->released, "write {} already released", index);
        (*it)->released = true;
        (*it)->gate.set_value();
    }

    void release_all() {
        for (auto& e : std::vector(live)) {
            if (!std::exchange(e->released, true)) {
                e->gate.set_value();
            }
        }
    }
};

class probe_file final : public ss::file_impl {
public:
    probe_file(ss::file f, inflight_write_probe& probe)
      : _f(std::move(f))
      , _probe(probe) {}

    ss::future<size_t> write_dma(
      uint64_t pos,
      const void* buffer,
      size_t len,
      ss::io_intent* intent) final {
        auto e = ss::make_lw_shared<inflight_write_probe::entry>();
        e->index = _probe.started++;
        e->pos = pos;
        e->buf = static_cast<const char*>(buffer);
        e->len = len;
        e->snapshot = ss::temporary_buffer<char>::aligned(
          inflight_write_probe::dma_alignment, len);
        std::memcpy(e->snapshot.get_write(), buffer, len);
        vlog(
          tst_log.debug,
          "[dma_write#{}] file_pos={} len={} buf={}",
          e->index,
          pos,
          len,
          fmt::ptr(buffer));

        _probe.check_alignment(*e);
        _probe.check_no_concurrent_overlap(*e);
        _probe.live.push_back(e);

        if (_probe.hold) {
            co_await e->gate.get_future();
        } else {
            for (size_t turns = _probe.delay_turns(); turns > 0; --turns) {
                co_await ss::yield();
            }
        }

        _probe.check_not_mutated(*e);

        auto written = co_await get_file_impl(_f)->write_dma(
          pos, e->snapshot.get(), len, intent);
        std::erase(_probe.live, e);
        co_return written;
    }

    ss::future<size_t>
    write_dma(uint64_t pos, std::vector<iovec> iov, ss::io_intent* i) final {
        return get_file_impl(_f)->write_dma(pos, std::move(iov), i);
    }
    ss::future<size_t>
    read_dma(uint64_t pos, void* buffer, size_t len, ss::io_intent* i) final {
        return get_file_impl(_f)->read_dma(pos, buffer, len, i);
    }
    ss::future<size_t>
    read_dma(uint64_t pos, std::vector<iovec> iov, ss::io_intent* i) final {
        return get_file_impl(_f)->read_dma(pos, std::move(iov), i);
    }
    ss::future<ss::temporary_buffer<uint8_t>>
    dma_read_bulk(uint64_t pos, size_t len, ss::io_intent* i) final {
        return get_file_impl(_f)->dma_read_bulk(pos, len, i);
    }
    ss::future<> flush() final { return get_file_impl(_f)->flush(); }
    ss::future<struct stat> stat() final { return get_file_impl(_f)->stat(); }
    ss::future<> truncate(uint64_t len) final {
        return get_file_impl(_f)->truncate(len);
    }
    ss::future<> discard(uint64_t pos, uint64_t len) final {
        return get_file_impl(_f)->discard(pos, len);
    }
    ss::future<> allocate(uint64_t pos, uint64_t len) final {
        return get_file_impl(_f)->allocate(pos, len);
    }
    ss::future<uint64_t> size() final { return get_file_impl(_f)->size(); }
    ss::future<> close() final { return get_file_impl(_f)->close(); }
    ss::subscription<ss::directory_entry> list_directory(
      std::function<ss::future<>(ss::directory_entry)> next) final {
        return get_file_impl(_f)->list_directory(std::move(next));
    }

private:
    ss::file _f;
    inflight_write_probe& _probe;
};

struct write_op {
    explicit write_op(size_t s)
      : size(s) {}
    explicit write_op(iobuf d)
      : data(std::move(d))
      , size(data->size_bytes()) {}
    std::optional<iobuf> data;
    size_t size;
};

struct flush_op {
    explicit flush_op(bool wait_for_flush)
      : wait_for_flush(wait_for_flush) {}
    bool wait_for_flush = false;
};

struct verify_op {};

struct truncate_op {
    explicit truncate_op(size_t n)
      : truncate_offset(n) {}
    size_t truncate_offset;
};

using operation = std::variant<write_op, flush_op, verify_op, truncate_op>;

struct SegmentAppenderFixture : seastar_test {
public:
    ss::future<> SetUpAsync() override {
        auto file = co_await ss::open_file_dma(
          "test_segment.log",
          ss::open_flags::rw | ss::open_flags::create
            | ss::open_flags::truncate,
          ss::file_open_options{});

        resources.start().get();
        storage::segment_appender::options opts(std::nullopt, resources, stats);
        appender = std::make_unique<storage::segment_appender>(
          wrap_file(std::move(file)), opts);
    }

    virtual ss::file wrap_file(ss::file f) { return f; }

    ss::future<> TearDownAsync() override {
        vlog(
          tst_log.debug,
          "Total appended size: {} bytes",
          reference.size_bytes());
        co_await ss::remove_file(file_name);
    }

    ss::future<> append_data(const char* data, size_t size) {
        co_await appender->append(data, size);
        reference.append(data, size);
    }

    ss::future<> append_data(const iobuf& data) {
        vlog(tst_log.debug, "Appending iobuf of size {}", data.size_bytes());
        co_await appender->append(data.copy());
        reference.append(data.copy());
    }

    ss::future<bool> file_content_equal_to_reference() {
        auto file = co_await ss::open_file_dma(
          file_name, ss::open_flags::ro, ss::file_open_options{});
        size_t file_size = co_await file.size();
        vassert(
          reference.size_bytes() == file_size,
          "File size {} does not match reference size {}",
          file_size,
          reference.size_bytes());

        ss::input_stream<char> in = ss::make_file_input_stream(
          std::move(file), 0, ss::file_input_stream_options{});
        auto ref_stream = make_iobuf_input_stream(reference.share());
        uint64_t offset = 0;
        while (!ref_stream.eof()) {
            auto ref_data = co_await ref_stream.read_exactly(4_KiB);
            auto file_data = co_await in.read_exactly(ref_data.size());
            vassert(
              ref_data == file_data, "Data mismatch at offset {}", offset);
            offset += ref_data.size();
        }
        co_return true;
    }

    ss::future<bool> reference_range_equal_to_file() {
        auto file = co_await ss::open_file_dma(
          file_name, ss::open_flags::ro, ss::file_open_options{});
        size_t file_size = co_await file.size();
        vassert(
          reference.size_bytes() <= file_size,
          "File size {} does not match reference size {}",
          file_size,
          reference.size_bytes());

        ss::input_stream<char> in = ss::make_file_input_stream(
          std::move(file), 0, ss::file_input_stream_options{});
        auto ref_stream = make_iobuf_input_stream(reference.share());
        uint64_t offset = 0;
        while (!ref_stream.eof()) {
            auto ref_data = co_await ref_stream.read_exactly(4_KiB);
            auto file_data = co_await in.read_exactly(ref_data.size());
            vassert(
              ref_data == file_data, "Data mismatch at offset {}", offset);
            offset += ref_data.size();
        }
        co_return true;
    }
    ss::future<> do_write(const write_op& w) {
        vlog(tst_log.debug, "[write] {} bytes", w.size);
        if (w.data) {
            co_await append_data(*w.data);
        } else {
            co_await append_data(tests::random_iobuf(w.size));
        }
    }

    ss::future<> execute_operation(operation op) {
        co_await ss::visit(
          op,
          [this](const write_op& w) { return do_write(w); },
          [this](const flush_op& f_op) {
              vlog(tst_log.debug, "[flush] wait: {}", f_op.wait_for_flush);
              auto f = ss::with_gate(
                gate, [this]() mutable { return appender->flush(); });
              if (f_op.wait_for_flush) {
                  return f;
              }

              return ss::now();
          },
          [this](const verify_op&) {
              vlog(tst_log.debug, "[verify]");
              return reference_range_equal_to_file().discard_result();
          },
          [this](const truncate_op& t_op) {
              vlog(tst_log.debug, "[truncate] size: {}", t_op.truncate_offset);
              auto to_trim = reference.size_bytes() - t_op.truncate_offset;
              reference.trim_back(to_trim);
              return appender->truncate(t_op.truncate_offset);
          });
    }

    ss::future<> execute_operations(chunked_vector<operation> ops) {
        for (auto& op : ops) {
            co_await execute_operation(std::move(op));
        }
    }

    ss::future<> execute_concurrent_flush_and_writes(
      size_t write_size, size_t total_bytes_to_write) {
        using namespace std::chrono_literals;
        bool writes_done = false;
        chunked_vector<ss::future<>> flush_futures;
        auto flusher = ss::do_until(
          [&] { return writes_done; },
          [&] {
              flush_futures.push_back(appender->flush());
              return ss::sleep(5us);
          });

        size_t counter = 0;
        auto writer = ss::do_until(
          [&] { return counter >= total_bytes_to_write; },
          [&] {
              return execute_operation(write_op(write_size)).then([&] {
                  counter += write_size;
              });
          });

        co_await std::move(writer);
        writes_done = true;
        co_await std::move(flusher);
        co_await ss::when_all_succeed(
          flush_futures.begin(), flush_futures.end());
    }

    std::string_view file_name = "test_segment.log";
    storage::storage_resources resources;
    ss::lw_shared_ptr<storage::segment_appender::stats> stats
      = ss::make_lw_shared<storage::segment_appender::stats>();
    std::unique_ptr<storage::segment_appender> appender;
    ss::gate gate;
    iobuf reference;
};

TEST_F(SegmentAppenderFixture, AppendMixedData) {
    chunked_vector<operation> ops;
    ops.emplace_back(write_op(64));
    ops.emplace_back(flush_op(true));
    ops.emplace_back(write_op(1024));
    ops.emplace_back(flush_op(true));
    ops.emplace_back(verify_op{});
    ops.emplace_back(write_op(12));
    ops.emplace_back(write_op(13));
    ops.emplace_back(flush_op(false));
    ops.emplace_back(write_op(45));
    ops.emplace_back(write_op(256));
    ops.emplace_back(flush_op(true));
    execute_operations(std::move(ops)).get();
    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
}

TEST_F(SegmentAppenderFixture, AppendAllSizesUpTo1MiB) {
    std::vector<operation> ops;
    for (auto i = 1; i <= 4096; i += 1) {
        execute_operation(write_op(i)).get();
    }

    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
}

TEST_F(SegmentAppenderFixture, TestLargeAppends) {
    std::vector<operation> ops;
    for (size_t i = 1; i <= 128 * 16_KiB; i += 16_KiB) {
        execute_operation(write_op(i)).get();
    }

    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
}

TEST_F(SegmentAppenderFixture, TestTruncation) {
    chunked_vector<operation> ops;
    // append 1 MiB in 64 KiB chunks
    for (size_t i = 0; i < 16; ++i) {
        ops.emplace_back(write_op(64_KiB));
    }
    ops.emplace_back(flush_op(true));
    // truncate to 512 KiB
    ops.emplace_back(truncate_op(512_KiB));
    ops.emplace_back(verify_op{});
    // append another 256 KiB
    for (size_t i = 0; i < 4; ++i) {
        ops.emplace_back(write_op(64_KiB));
    }

    execute_operations(std::move(ops)).get();
    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
}

TEST_F(SegmentAppenderFixture, TestFlushesAreMerged) {
    chunked_vector<operation> ops;
    // append 1 MiB in 16 KiB chunks with flushes in between
    for (size_t i = 0; i < 64; ++i) {
        ops.emplace_back(write_op(16_KiB));
    }
    for (auto i = 0; i < 64; ++i) {
        ops.emplace_back(flush_op(false));
    }
    ops.emplace_back(flush_op(true));
    execute_operations(std::move(ops)).get();
    EXPECT_GE(stats->fsyncs, 1);
    // TODO: fix possible redundant flushes in segment appender
    // EXPECT_LE(appender->get_stats().fsyncs, 2);
    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
}

TEST_F(SegmentAppenderFixture, TestConcurrentFlushes) {
    execute_concurrent_flush_and_writes(1, 16_KiB).get();
    ASSERT_GT(stats->bytes_copied_in_chunk_remainder, 0);
    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
    ASSERT_EQ(reference.size_bytes(), 16_KiB);
}

TEST_F(SegmentAppenderFixture, TestConcurrentFlushesPageBoundaryWrites) {
    execute_concurrent_flush_and_writes(4_KiB, 1_MiB).get();
    ASSERT_EQ(stats->bytes_copied_in_chunk_remainder, 0);
    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
    ASSERT_GE(reference.size_bytes(), 1_MiB);
}

TEST_F(SegmentAppenderFixture, TestConcurrentFlushesSmallWritesShifted) {
    // write 8 KiB to shift the chunk internal pointer
    execute_operation(write_op(8_KiB)).get();
    // now execute concurrent flushes with 1 byte writes
    execute_concurrent_flush_and_writes(1, 16_KiB).get();
    ASSERT_GT(stats->bytes_copied_in_chunk_remainder, 0);
    appender->close().get();
    ASSERT_TRUE(file_content_equal_to_reference().get());
    ASSERT_GE(reference.size_bytes(), 24_KiB);
}

/// Appends through a file that holds every dma write "in flight", either parked
/// until the test releases it or for a number of reactor turns, and checks the
/// appender's file-boundary invariants while it is in flight.
struct SegmentAppenderInflightFixture : SegmentAppenderFixture {
    ss::file wrap_file(ss::file f) override {
        return ss::file(ss::make_shared<probe_file>(std::move(f), probe));
    }

    ss::future<> wait_for_writes(size_t n) {
        for (size_t i = 0; i < 100000 && probe.started < n; ++i) {
            co_await ss::yield();
        }
        ASSERT_GE_CORO(probe.started, n);
    }

    void flush_nowait() { flushes.push_back(appender->flush()); }

    ss::future<> await_flushes() {
        co_await ss::when_all_succeed(flushes.begin(), flushes.end());
        flushes.clear();
    }

    // let every parked and future write through so close() can complete
    ss::future<> drain() {
        probe.hold = false;
        while (!probe.live.empty()) {
            probe.release_all();
            co_await ss::yield();
        }
        co_await await_flushes();
    }

    void report_failures() const {
        for (const auto& f : probe.failures) {
            ADD_FAILURE() << f;
        }
    }

    inflight_write_probe probe;
    std::vector<ss::future<>> flushes;
};

/*
 * A dispatched write must not have its chunk appended to, otherwise the DMA
 * reads a buffer that is being mutated underneath it -- the last-page
 * corruption that 4b435b9021 set out to prevent.
 *
 * is_chunk_write_dispatched() only inspects _inflight.back(), so a newer QUEUED
 * write for the same chunk hides an older DISPATCHED one and the guard lets the
 * append through. Reaching that state:
 *
 *   append 4 KiB, flush   -> w0 covers chunk [0, 4096), DISPATCHED and parked
 *   append 100            -> head has a dispatched write, so the (empty)
 *                            remainder is copied into a fresh chunk C
 *   flush                 -> w1 covers C [0, 4096), QUEUED behind w0 on
 *                            _prev_head_write, which the copy does not exchange
 *   append 30             -> back() is w1/QUEUED so the append goes into C in
 *                            place; safe, no DMA is reading C yet
 *   release w0            -> w1 is dispatched, its DMA now reads C [0, 4096)
 *   flush                 -> w2 cannot merge into w1 (DISPATCHED), so it is a
 *                            new QUEUED entry, again on C
 *   append 30             -> back() is w2/QUEUED, guard says "not dispatched",
 *                            and the append lands inside w1's live DMA range
 */
TEST_F_CORO(
  SegmentAppenderInflightFixture, TestAppendDoesNotMutateInflightDma) {
    co_await append_data(tests::random_iobuf(4_KiB));
    flush_nowait();
    co_await wait_for_writes(1);

    co_await append_data(tests::random_iobuf(100));
    flush_nowait();
    co_await ss::yield();
    ASSERT_EQ_CORO(probe.started, 1);

    co_await append_data(tests::random_iobuf(30));

    probe.release(0);
    co_await wait_for_writes(2);

    flush_nowait();
    co_await ss::yield();
    ASSERT_EQ_CORO(probe.started, 2);

    co_await append_data(tests::random_iobuf(30));

    co_await drain();
    co_await appender->close();

    report_failures();
    // the file contents are still correct: the mutated page is rewritten by the
    // following write, which is why no content check can detect this
    ASSERT_TRUE_CORO(co_await file_content_equal_to_reference());
}

/*
 * The same invariants, driven by a random workload rather than a hand-built
 * interleaving. Every write stays in flight for a random number of reactor
 * turns, so appends, flushes and truncations land at varying points relative to
 * the writes they race with.
 *
 * The seed is fixed so a failure is reproducible; override it to explore more
 * schedules:
 *
 *   bazel test //src/v/storage/tests:segment_appender_test \
 *     --test_env=SEGMENT_APPENDER_FUZZ_SEED=<n> \
 *     --test_env=SEGMENT_APPENDER_FUZZ_ITERS=<n> \
 *     --test_arg=--gtest_filter='*RandomWorkload*'
 */
TEST_F_CORO(
  SegmentAppenderInflightFixture, TestInflightInvariantsRandomWorkload) {
    auto env = [](const char* name, uint64_t fallback) {
        const char* v = std::getenv(name);
        return v != nullptr ? std::stoull(v) : fallback;
    };
    const uint64_t seed = env("SEGMENT_APPENDER_FUZZ_SEED", 20260727);
    const uint64_t iterations = env("SEGMENT_APPENDER_FUZZ_ITERS", 1000);

    std::mt19937_64 rng(seed);
    probe.hold = false;
    probe.delay_turns = [&rng] { return rng() % 4; };

    // a spread of sizes so the head chunk lands both on and off page boundaries
    const std::array sizes{
      1UL,
      7UL,
      30UL,
      100UL,
      512UL,
      1000UL,
      4095UL,
      4_KiB,
      4097UL,
      8_KiB,
      12000UL,
      16_KiB,
      20000UL};

    for (uint64_t i = 0; i < iterations; ++i) {
        co_await append_data(tests::random_iobuf(sizes[rng() % sizes.size()]));

        // an unawaited flush is what stacks up several _inflight entries
        if (rng() % 3 == 0) {
            flush_nowait();
        }
        if (rng() % 40 == 0 && reference.size_bytes() > 8_KiB) {
            // truncating to a usually unaligned offset leaves the rehydrated
            // head with an unaligned flushed position
            co_await await_flushes();
            const auto keep = reference.size_bytes() - (rng() % 8_KiB);
            co_await execute_operation(truncate_op(keep));
        }
        if (rng() % 4 == 0) {
            co_await ss::yield();
        }
    }

    co_await drain();
    co_await appender->close();

    vlog(
      tst_log.info,
      "seed={} iterations={} bytes={} writes={} failures={} "
      "remainder_copied={} "
      "merged={} split={}",
      seed,
      iterations,
      reference.size_bytes(),
      probe.started,
      probe.failures.size(),
      stats->bytes_copied_in_chunk_remainder,
      stats->merged_writes,
      stats->split_writes);

    report_failures();
    ASSERT_TRUE_CORO(co_await file_content_equal_to_reference());
}

/*
 * Two page-aligned writes on the same chunk cover disjoint pages: w0 reads
 * chunk [0, 4096) and writes file [0, 4096), w1 reads chunk [4096, 8192) and
 * writes file [4096, 8192). Nothing about the data requires them to be ordered,
 * so they could in principle be in flight together.
 *
 * They are not. Every write for a chunk takes a unit of the same
 * _prev_head_write semaphore before it is handed to the file, and holds it
 * until its completion has run, so while w0 is parked w1 never reaches the file
 * at all. That is what lets the appender record the in-flight dma extent on the
 * chunk: there is only ever one to record.
 */
TEST_F_CORO(
  SegmentAppenderInflightFixture, TestSameChunkWritesAreNeverConcurrent) {
    co_await append_data(tests::random_iobuf(4_KiB));
    flush_nowait();
    co_await wait_for_writes(1);
    ASSERT_EQ_CORO(probe.live.size(), 1);

    // page-aligned, so this accumulates into the same chunk rather than being
    // copied out to a fresh one
    co_await append_data(tests::random_iobuf(4_KiB));
    flush_nowait();

    // give w1 every opportunity to reach the file while w0 is still in flight
    for (int i = 0; i < 100; ++i) {
        co_await ss::yield();
    }
    ASSERT_EQ_CORO(probe.started, 1);
    ASSERT_EQ_CORO(probe.live.size(), 1);
    ASSERT_EQ_CORO(probe.live.front()->pos, 0);

    // w0 completing is what lets w1 through
    probe.release(0);
    co_await wait_for_writes(2);
    ASSERT_EQ_CORO(probe.live.size(), 1);
    ASSERT_EQ_CORO(probe.live.front()->pos, 4_KiB);

    co_await drain();
    co_await appender->close();

    report_failures();
    ASSERT_TRUE_CORO(co_await file_content_equal_to_reference());
}

using chunk = storage::segment_appender_chunk;
namespace {

chunk make_chunk(size_t chunk_size) {
    return chunk(chunk_size, storage::alignment(4_KiB));
}

size_t append_to_chunk(chunk& c, size_t size) {
    std::vector<char> data(size, '1');
    return c.append(data.data(), data.size());
}
} // namespace

TEST(SegmentAppenderChunk, test_copying_reminder) {
    auto chunk_1 = make_chunk(16_KiB);
    append_to_chunk(chunk_1, 10_KiB); // append 10 KiB
    auto chunk_2 = make_chunk(16_KiB);
    // whole chunk should be copied to chunk_2 as no data was flushed
    chunk_2.copy_remainder_from(chunk_1);

    ASSERT_EQ(chunk_2.size(), 10_KiB);

    chunk_1.flush();
    auto chunk_3 = make_chunk(16_KiB);
    // only last 2 KiB should be
    chunk_3.copy_remainder_from(chunk_1);

    ASSERT_EQ(chunk_3.size(), 2_KiB);
    ASSERT_EQ(chunk_3.flushed_pos(), 2_KiB);

    append_to_chunk(chunk_1, 3); // append 3 bytes
    auto chunk_4 = make_chunk(16_KiB);
    chunk_4.copy_remainder_from(chunk_1);
    // flushed position is preserved, only last 2 KiB + 3 bytes appended
    ASSERT_EQ(chunk_4.flushed_pos(), 2_KiB);
    ASSERT_EQ(chunk_4.size(), 2_KiB + 3);
}
