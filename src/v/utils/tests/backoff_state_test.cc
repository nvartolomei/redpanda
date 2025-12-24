// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "context/context_frame.h"
#include "context/deadline_timer.h"
#include "utils/backoff_state.h"

#include <seastar/core/sleep.hh>

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

} // namespace

TEST(BackoffStateTest, AttemptsExhausted) {
    constexpr uint16_t max_attempts = 5;
    utils::backoff_state bs{{.jitter = 0.0, .max_attempts = max_attempts}};

    context::context_frame<> ctx{context::background()};

    uint16_t count = 0;
    while (auto attempt = bs.backoff(ctx).get()) {
        EXPECT_EQ(*attempt, count);
        ++count;
    }

    EXPECT_EQ(count, max_attempts);

    // Further calls should return nullopt
    EXPECT_FALSE(bs.backoff(ctx).get().has_value());
}

TEST(BackoffStateTest, CancellationReturnsNullopt) {
    utils::backoff_state bs{{.jitter = 0.0, .max_attempts = 10}};

    context::context_frame<> ctx{context::background()};

    // First attempt succeeds
    auto first = bs.backoff(ctx).get();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0);

    // Cancel the context
    ctx.cancel_handle().trigger(context::cancel_cause::manual);

    // Next attempt should return nullopt
    EXPECT_FALSE(bs.backoff(ctx).get().has_value());
}

TEST(BackoffStateTest, DeadlineExpiryReturnsNullopt) {
    // Configure backoff with initial delay longer than context deadline
    utils::backoff_state bs{
      {.initial = 100ms, .jitter = 0.0, .max_attempts = 10}};

    using frame_t = context::context_frame<context::deadline_timer>;
    frame_t ctx{
      context::background(), context::with<context::deadline_timer>(5ms)};

    // First attempt returns immediately (no sleep)
    auto first = bs.backoff(ctx).get();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0);

    // Second attempt: delay (100ms) > time_left (~5ms), returns nullopt
    EXPECT_FALSE(bs.backoff(ctx).get().has_value());

    // Context may or may not be cancelled yet (deadline timer is async)
    // The key assertion is that backoff returned nullopt due to insufficient
    // time
}

TEST(BackoffStateTest, CancellationDuringSleepReturnsNullopt) {
    // Use short initial delay so the sleep actually happens
    utils::backoff_state bs{
      {.initial = 100ms, .jitter = 0.0, .max_attempts = 10}};

    context::context_frame<> ctx{context::background()};

    // First attempt returns immediately (no sleep)
    auto first = bs.backoff(ctx).get();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0);

    // Start second attempt (will sleep for ~100ms)
    auto fut = bs.backoff(ctx);

    // Wait briefly then cancel
    seastar::sleep(10ms).get();
    ctx.cancel_handle().trigger(context::cancel_cause::manual);

    // Should return nullopt via handle_exception_type path
    EXPECT_FALSE(fut.get().has_value());
    EXPECT_TRUE(ctx.is_cancelled());
}
