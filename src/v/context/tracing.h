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

#include <cstdint>
#include <limits>
#include <ranges>
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
struct tracing_span_data {
    tracing_id trace;
    tracing_span_id span;
    span_name name{"?"};
    const tracing_span_data* parent{nullptr};
    /// Fanout counter for generating child span IDs (like _fanout_id in
    /// retry_chain_node).
    mutable uint16_t next_child_id{0};

    /// Returns parent's span ID, or zero if root.
    [[nodiscard]] tracing_span_id parent_span() const noexcept {
        return parent ? parent->span : tracing_span_id{};
    }

    /// Create a child span (same trace_id, this span becomes parent).
    /// Child span ID is assigned from this span's fanout counter.
    /// \note Span IDs saturate at max value (65535) to avoid wraparound.
    [[nodiscard]] tracing_span_data child(span_name child_name) const noexcept {
        auto id = next_child_id;
        if (next_child_id < std::numeric_limits<uint16_t>::max()) [[likely]] {
            ++next_child_id;
        }
        return tracing_span_data{
          .trace = trace,
          .span = tracing_span_id{.value = id},
          .name = child_name,
          .parent = this,
        };
    }

    /// Create a root span with a new trace.
    /// Root spans always have span_id 0.
    static tracing_span_data create_root(span_name name) noexcept {
        return tracing_span_data{
          .trace = tracing_id::generate(),
          .span = tracing_span_id{.value = 0},
          .name = name,
          .parent = nullptr,
        };
    }
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
                  return fmt::format_to(
                    it, fmt::runtime(format), std::forward<Args>(args)...);
              });
            logger_->log(level, writer);
        }
    }

    ss::logger* logger_;
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
