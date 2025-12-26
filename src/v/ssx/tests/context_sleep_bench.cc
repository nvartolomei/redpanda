// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

/// \file context_sleep_bench.cc
/// \brief Benchmark comparing ssx::sleep vs seastar::sleep_abortable.

#include "context/context.h"
#include "context/context_frame.h"
#include "ssx/context_sleep.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/testing/perf_tests.hh>

namespace {

using test_clock = seastar::lowres_clock;

struct context_sleep_bench {};

} // namespace

// -----------------------------------------------------------------------------
// With timer (the common case)
// -----------------------------------------------------------------------------

PERF_TEST_C(context_sleep_bench, ssx_sleep_100) {
    context::context_frame<> frame{context::background()};
    constexpr auto dur = std::chrono::microseconds(1);

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        co_await ssx::sleep<test_clock>(frame, dur);
    }
    perf_tests::stop_measuring_time();
}

PERF_TEST_C(context_sleep_bench, seastar_sleep_abortable_100) {
    seastar::abort_source as;
    constexpr auto dur = std::chrono::microseconds(1);

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        co_await seastar::sleep_abortable<test_clock>(dur, as);
    }
    perf_tests::stop_measuring_time();
}

// -----------------------------------------------------------------------------
// Baseline: seastar::sleep (no cancellation support)
// -----------------------------------------------------------------------------

PERF_TEST_C(context_sleep_bench, seastar_sleep_100) {
    constexpr auto dur = std::chrono::microseconds(1);

    perf_tests::start_measuring_time();
    for (int i = 0; i < 100; ++i) {
        co_await seastar::sleep<test_clock>(dur);
    }
    perf_tests::stop_measuring_time();
}
