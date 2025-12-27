// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

/// \file tracing.h
/// \brief Tracing span support for context frames.
///
/// Provides distributed tracing primitives that integrate with the context
/// system. Spans automatically inherit trace IDs from ancestor frames,
/// enabling trace correlation across the call hierarchy.
///
/// ID generation uses a counter-based mechanism similar to retry_chain_node:
/// - Trace IDs: Generated from a thread-local counter for root spans
/// - Span IDs: Generated from a fanout counter in the parent span
///
/// This produces human-readable, hierarchical IDs useful for debugging.

#pragma once

#include "context/context.h"
#include "context/context_frame.h"

#include <seastar/core/sstring.hh>
#include <seastar/util/log.hh>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>

namespace context {

namespace detail {
/// Thread-local counter for generating root trace IDs.
/// Similar to fiber_count in retry_chain_node.
inline uint32_t& trace_counter() noexcept {
    thread_local uint32_t counter = 0;
    return counter;
}
} // namespace detail

/// Span name restricted to string literals.
class span_name {
public:
    span_name() = delete;

    template<size_t N>
    constexpr span_name(const char (&name)[N]) noexcept
      : view_(static_cast<const char*>(name), N - 1) {
        static_assert(N > 1, "span_name must not be empty");
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return view_;
    }

    /// NOLINTNEXTLINE(hicpp-explicit-conversions)
    constexpr operator std::string_view() const noexcept { return view_; }

private:
    std::string_view view_;
};

/// Trace identifier using a thread-local counter.
/// Similar to fiber_count in retry_chain_node.
struct tracing_id {
    uint32_t value{};

    static tracing_id generate() noexcept {
        return {.value = detail::trace_counter()++};
    }

    bool operator==(const tracing_id&) const = default;
};

/// Span identifier using a fanout counter from parent.
/// Similar to _fanout_id in retry_chain_node.
struct tracing_span_id {
    uint16_t value{};

    bool operator==(const tracing_span_id&) const = default;
};

/// Tracing span data containing trace and span identifiers.
///
/// ID generation uses a counter-based mechanism similar to retry_chain_node:
/// - Root spans get span_id 0
/// - Child spans get sequential IDs from parent's fanout counter
class tracing_span_data {
    friend class tracing_span;

public:
    tracing_id trace;
    tracing_span_id span;
    span_name name{"?"};
    const tracing_span_data* parent{nullptr};
    time_point start_time{clock::now()};

    /// Returns elapsed time since span creation.
    [[nodiscard]] duration elapsed() const noexcept {
        return clock::now() - start_time;
    }

private:
    tracing_span_data() = default;

    tracing_span_data(
      tracing_id t,
      tracing_span_id s,
      span_name n,
      const tracing_span_data* p) noexcept
      : trace(t)
      , span(s)
      , name(n)
      , parent(p)
      , start_time(clock::now()) {}

    /// Fanout counter for generating child span IDs (like _fanout_id in
    /// retry_chain_node).
    mutable uint16_t next_child_id{0};

    /// Create a child span (same trace_id, this span becomes parent).
    /// Child span ID is assigned from this span's fanout counter.
    /// \note Span IDs saturate at max value (65535) to avoid wraparound.
    [[nodiscard]] tracing_span_data child(span_name child_name) const noexcept {
        auto id = next_child_id;
        if (next_child_id < std::numeric_limits<uint16_t>::max()) [[likely]] {
            ++next_child_id;
        }
        return tracing_span_data{trace, {id}, child_name, this};
    }

    /// Create a root span with a new trace.
    /// Root spans always have span_id 0.
    static tracing_span_data create_root(span_name root_name) noexcept {
        return tracing_span_data{
          tracing_id::generate(), {0}, root_name, nullptr};
    }
};

/// Lazy span backtrace for zero-allocation formatting.
///
/// Holds a context_ref for debug reference counting and formats on demand.
/// Use with fmt::format or format_to() to write directly into an output buffer.
///
/// Example:
/// ```cpp
/// auto bt = ctx.span_backtrace();
/// fmt::format_to(buf, "error at: {}", bt);
/// ```
class span_backtrace {
public:
    explicit span_backtrace(context_ref ctx) noexcept
      : ctx_(ctx) {}

    /// Format into an output iterator. Returns the iterator past the end.
    template<typename OutputIt>
    OutputIt format_to(OutputIt out) const {
        const tracing_span_data* span = ctx_.trace_span();
        const tracing_span_data* current = span;
        while (current) {
            if (current != span) {
                out = std::copy_n(" <- ", 4, out);
            }
            auto name = current->name.view();
            out = std::copy(name.begin(), name.end(), out);
            current = current->parent;
        }
        return out;
    }

    /// Returns true if there's no span data.
    [[nodiscard]] bool empty() const noexcept {
        return ctx_.trace_span() == nullptr;
    }

    /// Returns the underlying span pointer.
    [[nodiscard]] const tracing_span_data* span() const noexcept {
        return ctx_.trace_span();
    }

private:
    /// Stored as context_ref (not raw pointer) to enable debug ref-counting
    /// assertions that catch use-after-frame-destruction.
    context_ref ctx_;
};

/// Tracing span mixin for context_frame.
///
/// Usage:
/// ```cpp
/// context::context_frame<context::tracing_span> frame{
///     parent,
///     context::with<context::tracing_span>(context::span_name{"operation_name"})
/// };
/// ```
class tracing_span {
    template<typename...>
    friend class context_frame;

public:
    tracing_span() = default;

    const tracing_span_data& data() const noexcept { return data_; }

private:
    /// Hook: initialize with operation name.
    /// Uses cached trace span pointer from parent (inherited during frame
    /// construction) to avoid virtual call overhead, then updates the cache
    /// to point to this span.
    template<typename Self>
    void on_context_init(this Self& self, span_name name) noexcept {
        // Use cached pointer (already inherited from parent during
        // basic_context_frame construction)
        if (auto* s = self.cached_trace_span_) {
            self.data_ = s->child(name);
        } else {
            self.data_ = tracing_span_data::create_root(name);
        }
        // Update cache to point to this span for descendants
        self.set_cached_trace_span(&self.data_);
    }

    tracing_span_data data_;
};

/// Trace-aware logger wrapper.
///
/// Caches the formatted span lineage prefix on first log call, making
/// subsequent calls efficient. Falls back to regular logging when no
/// trace context is available.
///
/// \note Stores a context_ref internally to leverage debug ref-counting
/// assertions. This catches use-after-frame-destruction in debug builds,
/// which is critical for coroutine safety where loggers may outlive frames.
class trace_logger {
public:
    trace_logger(context_ref ctx, ss::logger& logger) noexcept
      : logger_(&logger)
      , ctx_(ctx) {}

    template<typename... Args>
    void trace(const char* fmt, Args&&... args) const {
        log(ss::log_level::trace, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(const char* fmt, Args&&... args) const {
        log(ss::log_level::debug, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(const char* fmt, Args&&... args) const {
        log(ss::log_level::info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(const char* fmt, Args&&... args) const {
        log(ss::log_level::warn, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(const char* fmt, Args&&... args) const {
        log(ss::log_level::error, fmt, std::forward<Args>(args)...);
    }

private:
    // Prefix format: "[{trace_id}~{span_id}~...~{span_id} {name}] "
    // 128 bytes covers typical use cases without heap allocation.
    static constexpr size_t prefix_buf_size = 128;

    /// Returns the cached prefix, formatting it on first call.
    std::string_view prefix() const;

    template<typename... Args>
    void log(ss::log_level level, const char* format, Args&&... args) const {
        if (logger_->is_enabled(level)) [[unlikely]] {
            ss::logger::lambda_log_writer writer(
              [&](ss::internal::log_buf::inserter_iterator it) {
                  it = std::ranges::copy(prefix(), it).out;
                  // Append elapsed + closing bracket
                  if (auto* span = ctx_.trace_span()) {
                      if (!ctx_.has_deadline()) {
                          *it++ = ' ';
                      }
                      it = format_duration(it, span->elapsed());
                      it = std::copy_n("] ", 2, it);
                  }
                  return fmt::format_to(
                    it, fmt::runtime(format), std::forward<Args>(args)...);
              });
            logger_->log(level, writer);
        }
    }

    /// Formats duration in human-readable form (integer ms, s, or m).
    template<typename OutputIt>
    static OutputIt format_duration(OutputIt it, duration d) {
        auto ms
          = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
        if (ms >= 60'000) {
            return fmt::format_to(it, "{}m", ms / 60'000);
        } else if (ms >= 1'000) {
            return fmt::format_to(it, "{}s", ms / 1'000);
        } else {
            return fmt::format_to(it, "{}ms", ms);
        }
    }

    ss::logger* logger_;
    /// Stored as context_ref (not raw pointer) to enable debug ref-counting
    /// assertions that catch use-after-frame-destruction.
    context_ref ctx_;
    mutable fmt::basic_memory_buffer<char, prefix_buf_size> prefix_buf_;
};

/// Create a trace-aware logger.
///
/// The returned logger caches trace context, making it efficient for
/// multiple log calls within the same scope.
[[nodiscard]] inline trace_logger make_trace_logger(
  context_ref ctx [[clang::lifetimebound]], ss::logger& logger) noexcept {
    return trace_logger{ctx, logger};
}

} // namespace context

template<>
struct fmt::formatter<context::span_backtrace> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template<typename FormatContext>
    auto format(const context::span_backtrace& bt, FormatContext& ctx) const {
        return bt.format_to(ctx.out());
    }
};
