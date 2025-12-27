// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "context/context_debug.h"
#include "context/linker.h"
#include "context/tracing.h"

#include <gtest/gtest.h>

namespace {

ss::logger test_logger("span_test");

TEST(SpanTest, TraceIdGeneration) {
    auto id1 = context::tracing_id::generate();
    auto id2 = context::tracing_id::generate();
    EXPECT_NE(id1, id2);
    EXPECT_EQ(id2.value, id1.value + 1);
}

TEST(SpanTest, RootSpanCreation) {
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(
        context::span_name{"root_operation"})};

    auto* span = root.trace_span();
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(span->name.view(), "root_operation");
    EXPECT_EQ(span->parent, nullptr);
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

    EXPECT_EQ(child_span->trace, root_span->trace);
    EXPECT_EQ(child_span->parent, root_span);
    EXPECT_EQ(child_span->name.view(), "child");
}

TEST(SpanTest, InheritanceAcrossGaps) {
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"root"})};
    context::context_frame<> middle{root};
    context::context_frame<context::tracing_span> grandchild{
      middle,
      context::with<context::tracing_span>(context::span_name{"grandchild"})};

    auto* root_span = root.trace_span();
    auto* grandchild_span = grandchild.trace_span();
    ASSERT_NE(root_span, nullptr);
    ASSERT_NE(grandchild_span, nullptr);

    EXPECT_EQ(grandchild_span->trace, root_span->trace);
    EXPECT_EQ(grandchild_span->parent, root_span);
}

TEST(SpanTest, LoggingAcrossGaps) {
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"root"})};
    context::context_frame<> middle{root};
    context::context_frame<> leaf{context_ref{middle}};

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

    auto tlog = context::make_trace_logger(grandchild, test_logger);
    tlog.info("message from grandchild");
}

TEST(SpanTest, TraceLoggerWithoutSpan) {
    context::context_frame<> frame{context::background()};
    auto tlog = context::make_trace_logger(frame, test_logger);
    tlog.info("test message without span");
}

TEST(SpanTest, DumpTree) {
    context::context_frame<context::tracing_span> server{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"server"})};

    context::context_frame<context::tracing_span> request_1{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"request_1"})};
    auto request_1_link = context::link(request_1, server);

    context::context_frame<context::tracing_span> db_query{
      request_1,
      context::with<context::tracing_span>(context::span_name{"db_query"})};

    context::context_frame<context::tracing_span> request_2{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"request_2"})};
    auto request_2_link = context::link(request_2, server);
    request_2.cancel_handle().trigger(context::cancel_cause::manual);

    auto tree_str = context::experimental::dump_tree_string(server);
    EXPECT_NE(tree_str.find("server"), ss::sstring::npos);
    EXPECT_NE(tree_str.find("linked"), ss::sstring::npos);

    auto req1_str = context::experimental::dump_tree_string(request_1);
    EXPECT_NE(req1_str.find("request_1"), ss::sstring::npos);
    EXPECT_NE(req1_str.find("db_query"), ss::sstring::npos);
}

TEST(SpanTest, DumpTreeTruncation) {
    context::context_frame<context::tracing_span> root{
      context::background(),
      context::with<context::tracing_span>(context::span_name{"root"})};

    std::vector<std::unique_ptr<context::context_frame<context::tracing_span>>>
      children;
    for (int i = 0; i < 15; ++i) {
        children.push_back(
          std::make_unique<context::context_frame<context::tracing_span>>(
            root,
            context::with<context::tracing_span>(context::span_name{"child"})));
    }

    auto tree_str = context::experimental::dump_tree_string(root);
    EXPECT_NE(tree_str.find("... and 5 more"), ss::sstring::npos);

    size_t count = 0;
    for (size_t pos = 0;
         (pos = tree_str.find("child", pos)) != ss::sstring::npos;
         ++pos) {
        ++count;
    }
    EXPECT_EQ(count, 10);
}

TEST(SpanTest, SpanBacktrace) {
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

    context_ref gc_ref = grandchild;
    EXPECT_EQ(
      fmt::format("{}", gc_ref.span_backtrace()),
      "grandchild_op <- child_op <- root_op");
    EXPECT_FALSE(gc_ref.span_backtrace().empty());

    context_ref child_ref = child;
    EXPECT_EQ(
      fmt::format("{}", child_ref.span_backtrace()), "child_op <- root_op");

    context_ref root_ref = root;
    EXPECT_EQ(fmt::format("{}", root_ref.span_backtrace()), "root_op");

    context::context_frame<> no_span{context::background()};
    context_ref no_span_ref = no_span;
    EXPECT_TRUE(no_span_ref.span_backtrace().empty());
    EXPECT_EQ(fmt::format("{}", no_span_ref.span_backtrace()), "");
}

} // namespace
