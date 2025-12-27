// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

/// \file context_debug.h
/// \brief Experimental debugging utilities for context hierarchies.

#pragma once

#include "context/context.h"
#include "context/tracing.h"

#include <seastar/core/sstring.hh>

#include <string>
#include <string_view>

namespace context::experimental {

namespace detail {

inline std::string_view
get_span_name(const context::detail::basic_context_frame& frame) {
    if (auto* span = frame.trace_span()) {
        return span->name;
    }
    return "?";
}

inline void append_cancel_status(
  std::string& out, const context::detail::basic_context_frame& frame) {
    if (!frame.is_cancelled()) {
        return;
    }
    out.append(" [cancelled:");
    switch (frame.cancel_cause()) {
    case cancel_cause::manual:
        out.append("manual");
        break;
    case cancel_cause::deadline:
        out.append("deadline");
        break;
    default:
        out.append("?");
        break;
    }
    out.append("]");
}

template<typename OutputFn>
void dump_tree_impl(
  const context::detail::basic_context_frame& frame,
  OutputFn& out,
  std::string& line_buf,
  std::string_view indent,
  size_t max_children) {
    line_buf.clear();
    line_buf.append(indent);
    line_buf.append(get_span_name(frame));
    append_cancel_status(line_buf, frame);
    out(line_buf);

    std::string child_indent{indent};
    child_indent.append("  ");

    size_t shown = 0;
    size_t total = 0;
    frame.for_each_child([&](
                           const context::detail::basic_context_frame& child) {
        if (auto* source = child.linked_source()) {
            ++total;
            if (max_children > 0 && shown >= max_children) {
                return;
            }
            ++shown;

            line_buf.clear();
            line_buf.append(child_indent);
            line_buf.append("<- linked: ");
            line_buf.append(get_span_name(*source));
            append_cancel_status(line_buf, *source);
            out(line_buf);

            std::string nested_indent{child_indent};
            nested_indent.append("            ");

            size_t nested_shown = 0;
            size_t nested_total = 0;
            source->for_each_child(
              [&](const context::detail::basic_context_frame& c) {
                  if (c.linked_source() || c.has_links()) {
                      return;
                  }
                  ++nested_total;
                  if (max_children > 0 && nested_shown >= max_children) {
                      return;
                  }
                  ++nested_shown;
                  dump_tree_impl(c, out, line_buf, nested_indent, max_children);
              });
            if (nested_total > nested_shown) {
                out(
                  nested_indent + "... and "
                  + std::to_string(nested_total - nested_shown) + " more");
            }
        } else {
            if (child.has_links()) {
                return;
            }
            ++total;
            if (max_children > 0 && shown >= max_children) {
                return;
            }
            ++shown;
            dump_tree_impl(child, out, line_buf, child_indent, max_children);
        }
    });
    if (total > shown) {
        out(
          child_indent + "... and " + std::to_string(total - shown) + " more");
    }
}

} // namespace detail

/// Dumps the context tree starting from the given context.
template<typename OutputFn>
void dump_tree(
  context_ref ctx,
  OutputFn&& out,
  std::string_view indent = "",
  size_t max_children = 10) {
    std::string line_buf;
    line_buf.append(indent);
    if (auto* span = ctx.trace_span()) {
        line_buf.append(span->name);
    } else {
        line_buf.append("?");
    }
    if (ctx.is_cancelled()) {
        line_buf.append(" [cancelled:");
        switch (ctx.cancel_cause()) {
        case cancel_cause::manual:
            line_buf.append("manual");
            break;
        case cancel_cause::deadline:
            line_buf.append("deadline");
            break;
        default:
            line_buf.append("?");
            break;
        }
        line_buf.append("]");
    }
    out(line_buf);

    std::string child_indent{indent};
    child_indent.append("  ");

    size_t shown = 0;
    size_t total = 0;
    ctx.for_each_child([&](const context::detail::basic_context_frame& child) {
        if (auto* source = child.linked_source()) {
            ++total;
            if (max_children > 0 && shown >= max_children) {
                return;
            }
            ++shown;

            line_buf.clear();
            line_buf.append(child_indent);
            line_buf.append("<- linked: ");
            line_buf.append(detail::get_span_name(*source));
            detail::append_cancel_status(line_buf, *source);
            out(line_buf);

            std::string nested_indent{child_indent};
            nested_indent.append("            ");

            size_t nested_shown = 0;
            size_t nested_total = 0;
            source->for_each_child(
              [&](const context::detail::basic_context_frame& c) {
                  if (c.linked_source() || c.has_links()) {
                      return;
                  }
                  ++nested_total;
                  if (max_children > 0 && nested_shown >= max_children) {
                      return;
                  }
                  ++nested_shown;
                  detail::dump_tree_impl(
                    c, out, line_buf, nested_indent, max_children);
              });
            if (nested_total > nested_shown) {
                out(
                  nested_indent + "... and "
                  + std::to_string(nested_total - nested_shown) + " more");
            }
        } else {
            if (child.has_links()) {
                return;
            }
            ++total;
            if (max_children > 0 && shown >= max_children) {
                return;
            }
            ++shown;
            detail::dump_tree_impl(
              child, out, line_buf, child_indent, max_children);
        }
    });
    if (total > shown) {
        out(
          child_indent + "... and " + std::to_string(total - shown) + " more");
    }
}

/// Dumps the context tree to a string.
[[nodiscard]] inline ss::sstring dump_tree_string(context_ref ctx) {
    ss::sstring result;
    dump_tree(ctx, [&](std::string_view line) {
        result.append(line.data(), line.size());
        result.append("\n", 1);
    });
    return result;
}

} // namespace context::experimental
