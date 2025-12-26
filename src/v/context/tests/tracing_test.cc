// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "context/tracing.h"

#include <gtest/gtest.h>

namespace {

// Test logger for capturing output
ss::logger test_logger("span_test");

TEST(SpanTest, TraceIdGeneration) {
    auto id1 = context::tracing_id::generate();
    auto id2 = context::tracing_id::generate();

    // Generated IDs should be sequential (counter-based)
    EXPECT_NE(id1, id2);
    EXPECT_EQ(id2.value, id1.value + 1);
}

TEST(SpanTest, SpanIdFromFanout) {
    // Span IDs come from parent's fanout counter (like retry_chain_node)
    auto root = context::tracing_span_data::create_root(
      context::span_name{"root"});
    auto child1 = root.child(context::span_name{"child1"});
    auto child2 = root.child(context::span_name{"child2"});

    // Root span always has ID 0
    EXPECT_EQ(root.span.value, 0);

    // Children get sequential IDs from parent's fanout counter
    EXPECT_EQ(child1.span.value, 0);
    EXPECT_EQ(child2.span.value, 1);
}

TEST(SpanTest, RootSpanCreation) {
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(
        context::span_name{"root_operation"})};

    auto* span = root.trace_span();
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(span->name.view(), "root_operation");

    // Root span should have no parent span
    context::tracing_span_id zero_span{};
    EXPECT_EQ(span->parent_span(), zero_span);
}

TEST(SpanTest, ChildSpanInheritance) {
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"root"})};

    context::context_frame<context::tracing_span> child{
      root, context::with<context::tracing_span>(context::span_name{"child"})};

    auto* root_span = root.trace_span();
    auto* child_span = child.trace_span();

    ASSERT_NE(root_span, nullptr);
    ASSERT_NE(child_span, nullptr);

    // Child should inherit tracing_id
    EXPECT_EQ(child_span->trace, root_span->trace);

    // Child's parent_span should be root's span
    EXPECT_EQ(child_span->parent_span(), root_span->span);

    // Child has different parent pointer (distinguishes hierarchy)
    EXPECT_NE(child_span->parent, nullptr);
    EXPECT_EQ(child_span->parent, root_span);

    EXPECT_EQ(child_span->name.view(), "child");
}

TEST(SpanTest, InheritanceAcrossGaps) {
    // Root has span
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"root"})};

    // Middle has no span mixin
    context::context_frame<> middle{root};

    // Grandchild has span - should inherit from root
    context::context_frame<context::tracing_span> grandchild{
      middle,
      context::with<context::tracing_span>(context::span_name{"grandchild"})};

    auto* root_span = root.trace_span();
    auto* grandchild_span = grandchild.trace_span();

    ASSERT_NE(root_span, nullptr);
    ASSERT_NE(grandchild_span, nullptr);

    // Grandchild should inherit tracing_id from root
    EXPECT_EQ(grandchild_span->trace, root_span->trace);

    // Grandchild's parent should be root's span (skipping middle)
    EXPECT_EQ(grandchild_span->parent_span(), root_span->span);
}

TEST(SpanTest, LoggingAcrossGaps) {
    // Root has span
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"root"})};

    // Middle has no span
    context::context_frame<> middle{static_cast<context_ref>(root)};

    // Leaf has no span
    context::context_frame<> leaf{static_cast<context_ref>(middle)};

    // trace_span() on leaf should find root's span
    context_ref leaf_ref = leaf;
    auto* found_span = leaf_ref.trace_span();

    ASSERT_NE(found_span, nullptr);
    EXPECT_EQ(found_span->trace, root.trace_span()->trace);
    EXPECT_EQ(found_span->name.view(), "root");
}

TEST(SpanTest, NoSpanReturnsNullptr) {
    context::context_frame<> frame{context::background()};

    EXPECT_EQ(frame.trace_span(), nullptr);

    context_ref ref = frame;
    EXPECT_EQ(ref.trace_span(), nullptr);
}

TEST(SpanTest, TraceLoggerWithSpan) {
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"root_op"})};

    context::context_frame<context::tracing_span> child{
      root,
      context::with<context::tracing_span>(context::span_name{"child_op"})};

    context::context_frame<context::tracing_span> grandchild{
      child,
      context::with<context::tracing_span>(
        context::span_name{"grandchild_op"})};

    // Log from grandchild - should show full lineage
    auto tlog = context::make_trace_logger(grandchild, test_logger);
    tlog.info("message from grandchild");
}

TEST(SpanTest, TraceLoggerWithoutSpan) {
    context::context_frame<> frame{context::background()};

    auto tlog = context::make_trace_logger(frame, test_logger);

    // Should work without span - just no prefix
    tlog.info("test message without span");
}

TEST(SpanTest, SpanDataChild) {
    auto root = context::tracing_span_data::create_root(
      context::span_name{"root"});
    auto child = root.child(context::span_name{"child"});

    EXPECT_EQ(child.trace, root.trace);
    EXPECT_EQ(child.parent_span(), root.span);
    // Child is distinguished by parent pointer, not span ID alone
    EXPECT_EQ(child.parent, &root);
    EXPECT_EQ(child.name.view(), "child");
}

} // namespace
