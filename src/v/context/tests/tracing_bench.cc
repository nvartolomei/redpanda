// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

/// \file tracing_bench.cc
/// \brief Benchmark comparing context tracing system vs retry_chain_node.
///
/// Compares:
/// - Frame/node construction overhead
/// - Logger prefix formatting
/// - Hierarchy (tree) construction
/// - Logging with context

#include "context/context.h"
#include "context/context_frame.h"
#include "context/tracing.h"
#include "utils/retry_chain_node.h"

#include <seastar/testing/perf_tests.hh>
#include <seastar/util/log.hh>

#include <array>
#include <optional>

namespace {

static ss::logger bench_log("bench");

// Disable actual logging output during benchmarks
struct log_level_guard {
    log_level_guard() { bench_log.set_level(ss::log_level::error); }
    ~log_level_guard() { bench_log.set_level(ss::log_level::info); }
};

// Frame type with tracing span
using traced_frame = context::context_frame<context::tracing_span>;

// --------------------------------------------------------------------------
// Single frame/node construction benchmarks
// --------------------------------------------------------------------------

// Baseline: measure loop overhead
PERF_TEST(tracing, baseline_empty_loop) {
    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        perf_tests::do_not_optimize(i);
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// Context frame without tracing (minimal overhead)
PERF_TEST(tracing, context_frame_create_1000) {
    using frame_t = context::context_frame<>;
    std::optional<frame_t> frame;

    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        frame.emplace(context::background());
        perf_tests::do_not_optimize(frame);
        frame.reset();
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// Context frame with tracing span
PERF_TEST(tracing, traced_frame_create_1000) {
    std::optional<traced_frame> frame;

    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        frame.emplace(
          context::background(),
          context::with<context::tracing_span>("test_op"));
        perf_tests::do_not_optimize(frame);
        frame.reset();
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// retry_chain_node child construction (fair comparison with context_frame)
PERF_TEST(tracing, retry_chain_node_create_1000) {
    ss::abort_source as;
    retry_chain_node root{as};
    std::optional<retry_chain_node> node;

    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        node.emplace(&root);
        perf_tests::do_not_optimize(node);
        node.reset();
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// retry_chain_node with deadline
PERF_TEST(tracing, retry_chain_node_with_deadline_create_1000) {
    ss::abort_source as;
    std::optional<retry_chain_node> node;
    auto deadline = ss::lowres_clock::now() + std::chrono::seconds(60);

    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        node.emplace(as, deadline, std::chrono::milliseconds(100));
        perf_tests::do_not_optimize(node);
        node.reset();
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// --------------------------------------------------------------------------
// Hierarchy construction benchmarks
// Note: retry_chain_node has max depth of 8, so we use depth 3 + fanout
// --------------------------------------------------------------------------

// Context frame tree (without tracing) - depth 3, fanout 20 = 63 nodes
PERF_TEST(tracing, context_tree_depth3_fanout20) {
    using frame_t = context::context_frame<>;
    std::array<std::optional<frame_t>, 3> spine;
    std::array<std::optional<frame_t>, 20> fanout;
    std::array<std::array<std::optional<frame_t>, 2>, 20> tails;

    perf_tests::start_measuring_time();

    // Build spine (depth 3)
    spine[0].emplace(context::background());
    for (size_t i = 1; i < 3; ++i) {
        spine[i].emplace(context_ref{*spine[i - 1]});
    }
    // Build fanout from last spine node (depth 4)
    for (size_t i = 0; i < 20; ++i) {
        fanout[i].emplace(context_ref{*spine[2]});
    }
    // Build tails from each fanout (depth 5-6)
    for (size_t i = 0; i < 20; ++i) {
        tails[i][0].emplace(context_ref{*fanout[i]});
        for (size_t j = 1; j < 2; ++j) {
            tails[i][j].emplace(context_ref{*tails[i][j - 1]});
        }
    }

    perf_tests::do_not_optimize(tails);
    perf_tests::stop_measuring_time();

    // Destroy in reverse order (LIFO)
    for (auto& tail : tails) {
        for (int j = 1; j >= 0; --j) {
            tail[j].reset();
        }
    }
    for (int i = 19; i >= 0; --i) {
        fanout[i].reset();
    }
    for (int i = 2; i >= 0; --i) {
        spine[i].reset();
    }
}

// Traced context frame tree
PERF_TEST(tracing, traced_context_tree_depth3_fanout20) {
    std::array<std::optional<traced_frame>, 3> spine;
    std::array<std::optional<traced_frame>, 20> fanout;
    std::array<std::array<std::optional<traced_frame>, 2>, 20> tails;

    perf_tests::start_measuring_time();

    // Build spine
    spine[0].emplace(
      context::background(), context::with<context::tracing_span>("root"));
    for (size_t i = 1; i < 3; ++i) {
        spine[i].emplace(
          context_ref{*spine[i - 1]},
          context::with<context::tracing_span>("spine"));
    }
    // Build fanout from last spine node
    for (size_t i = 0; i < 20; ++i) {
        fanout[i].emplace(
          context_ref{*spine[2]},
          context::with<context::tracing_span>("fanout"));
    }
    // Build tails from each fanout
    for (size_t i = 0; i < 20; ++i) {
        tails[i][0].emplace(
          context_ref{*fanout[i]},
          context::with<context::tracing_span>("tail"));
        for (size_t j = 1; j < 2; ++j) {
            tails[i][j].emplace(
              context_ref{*tails[i][j - 1]},
              context::with<context::tracing_span>("tail"));
        }
    }

    perf_tests::do_not_optimize(tails);
    perf_tests::stop_measuring_time();

    // Destroy in reverse order (LIFO)
    for (auto& tail : tails) {
        for (int j = 1; j >= 0; --j) {
            tail[j].reset();
        }
    }
    for (int i = 19; i >= 0; --i) {
        fanout[i].reset();
    }
    for (int i = 2; i >= 0; --i) {
        spine[i].reset();
    }
}

// retry_chain_node tree (max depth 7 to stay within limit of 8)
PERF_TEST(tracing, retry_chain_node_tree_depth3_fanout20) {
    ss::abort_source as;
    std::array<std::optional<retry_chain_node>, 3> spine;
    std::array<std::optional<retry_chain_node>, 20> fanout;
    std::array<std::array<std::optional<retry_chain_node>, 2>, 20> tails;

    perf_tests::start_measuring_time();

    // Build spine (depth 3)
    spine[0].emplace(as);
    for (size_t i = 1; i < 3; ++i) {
        spine[i].emplace(&*spine[i - 1]);
    }
    // Build fanout from last spine node (depth 4)
    for (size_t i = 0; i < 20; ++i) {
        fanout[i].emplace(&*spine[2]);
    }
    // Build tails from each fanout (depth 5-6)
    for (size_t i = 0; i < 20; ++i) {
        tails[i][0].emplace(&*fanout[i]);
        for (size_t j = 1; j < 2; ++j) {
            tails[i][j].emplace(&*tails[i][j - 1]);
        }
    }

    perf_tests::do_not_optimize(tails);
    perf_tests::stop_measuring_time();

    // Destroy in reverse order (LIFO)
    for (auto& tail : tails) {
        for (int j = 1; j >= 0; --j) {
            tail[j].reset();
        }
    }
    for (int i = 19; i >= 0; --i) {
        fanout[i].reset();
    }
    for (int i = 2; i >= 0; --i) {
        spine[i].reset();
    }
}

// --------------------------------------------------------------------------
// Logging benchmarks (actual log calls, output suppressed)
// --------------------------------------------------------------------------

// trace_logger logging (output suppressed)
PERF_TEST(tracing, trace_logger_log_1000) {
    log_level_guard guard;

    traced_frame frame{
      context::background(), context::with<context::tracing_span>("operation")};

    context::trace_logger logger{context_ref{frame}, bench_log};

    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        logger.info("test message {} with value {}", i, 42);
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// retry_chain_logger logging (output suppressed)
PERF_TEST(tracing, retry_chain_logger_log_1000) {
    log_level_guard guard;

    ss::abort_source as;
    retry_chain_node node{
      as,
      ss::lowres_clock::now() + std::chrono::seconds(60),
      std::chrono::milliseconds(100)};
    retry_chain_logger logger{bench_log, node};

    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        logger.info("test message {} with value {}", i, 42);
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// retry_chain_logger with custom context logging
PERF_TEST(tracing, retry_chain_logger_with_ctx_log_1000) {
    log_level_guard guard;

    ss::abort_source as;
    retry_chain_node node{
      as,
      ss::lowres_clock::now() + std::chrono::seconds(60),
      std::chrono::milliseconds(100)};
    retry_chain_logger logger{bench_log, node, "ns/topic/0"};

    perf_tests::start_measuring_time();
    for (int i = 0; i < 1000; ++i) {
        logger.info("test message {} with value {}", i, 42);
    }
    perf_tests::stop_measuring_time();
    return 1000;
}

// --------------------------------------------------------------------------
// Nested hierarchy with logging benchmarks
// --------------------------------------------------------------------------

// Create nested context with tracing and log at each level
PERF_TEST(tracing, traced_context_nested_with_logging_100) {
    log_level_guard guard;

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        traced_frame root{
          context::background(), context::with<context::tracing_span>("root")};
        context::trace_logger root_log{context_ref{root}, bench_log};
        root_log.info("root operation {}", i);

        traced_frame child{
          context_ref{root}, context::with<context::tracing_span>("child")};
        context::trace_logger child_log{context_ref{child}, bench_log};
        child_log.info("child operation {}", i);

        traced_frame grandchild{
          context_ref{child},
          context::with<context::tracing_span>("grandchild")};
        context::trace_logger grandchild_log{
          context_ref{grandchild}, bench_log};
        grandchild_log.info("grandchild operation {}", i);

        perf_tests::do_not_optimize(grandchild_log);
    }
    perf_tests::stop_measuring_time();
    return 100;
}

// Create nested retry_chain_node and log at each level
PERF_TEST(tracing, retry_chain_node_nested_with_logging_100) {
    log_level_guard guard;

    ss::abort_source as;

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        retry_chain_node root{as};
        retry_chain_logger root_log{bench_log, root};
        root_log.info("root operation {}", i);

        retry_chain_node child{&root};
        retry_chain_logger child_log{bench_log, child};
        child_log.info("child operation {}", i);

        retry_chain_node grandchild{&child};
        retry_chain_logger grandchild_log{bench_log, grandchild};
        grandchild_log.info("grandchild operation {}", i);

        perf_tests::do_not_optimize(grandchild_log);
    }
    perf_tests::stop_measuring_time();
    return 100;
}

// --------------------------------------------------------------------------
// Logging benchmarks (actual log output enabled)
// These measure real-world logging performance including I/O
// --------------------------------------------------------------------------

// trace_logger logging with output enabled
PERF_TEST(tracing, trace_logger_log_enabled_100) {
    traced_frame frame{
      context::background(), context::with<context::tracing_span>("operation")};

    context::trace_logger logger{context_ref{frame}, bench_log};

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        logger.info("test message {} with value {}", i, 42);
    }
    perf_tests::stop_measuring_time();
    return 100;
}

// retry_chain_logger logging with output enabled
PERF_TEST(tracing, retry_chain_logger_log_enabled_100) {
    ss::abort_source as;
    retry_chain_node node{
      as,
      ss::lowres_clock::now() + std::chrono::seconds(60),
      std::chrono::milliseconds(100)};
    retry_chain_logger logger{bench_log, node};

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        logger.info("test message {} with value {}", i, 42);
    }
    perf_tests::stop_measuring_time();
    return 100;
}

// retry_chain_logger with custom context, output enabled
PERF_TEST(tracing, retry_chain_logger_with_ctx_log_enabled_100) {
    ss::abort_source as;
    retry_chain_node node{
      as,
      ss::lowres_clock::now() + std::chrono::seconds(60),
      std::chrono::milliseconds(100)};
    retry_chain_logger logger{bench_log, node, "ns/topic/0"};

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        logger.info("test message {} with value {}", i, 42);
    }
    perf_tests::stop_measuring_time();
    return 100;
}

// Nested hierarchy with actual logging output
PERF_TEST(tracing, traced_context_nested_log_enabled_100) {
    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        traced_frame root{
          context::background(), context::with<context::tracing_span>("root")};
        context::trace_logger root_log{context_ref{root}, bench_log};
        root_log.info("root operation {}", i);

        traced_frame child{
          context_ref{root}, context::with<context::tracing_span>("child")};
        context::trace_logger child_log{context_ref{child}, bench_log};
        child_log.info("child operation {}", i);

        traced_frame grandchild{
          context_ref{child},
          context::with<context::tracing_span>("grandchild")};
        context::trace_logger grandchild_log{
          context_ref{grandchild}, bench_log};
        grandchild_log.info("grandchild operation {}", i);

        perf_tests::do_not_optimize(grandchild_log);
    }
    perf_tests::stop_measuring_time();
    return 100;
}

// Nested retry_chain_node with actual logging output
PERF_TEST(tracing, retry_chain_node_nested_log_enabled_100) {
    ss::abort_source as;

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        retry_chain_node root{as};
        retry_chain_logger root_log{bench_log, root};
        root_log.info("root operation {}", i);

        retry_chain_node child{&root};
        retry_chain_logger child_log{bench_log, child};
        child_log.info("child operation {}", i);

        retry_chain_node grandchild{&child};
        retry_chain_logger grandchild_log{bench_log, grandchild};
        grandchild_log.info("grandchild operation {}", i);

        perf_tests::do_not_optimize(grandchild_log);
    }
    perf_tests::stop_measuring_time();
    return 100;
}

} // namespace
