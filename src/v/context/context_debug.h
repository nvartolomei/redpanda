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

namespace context::experimental {

/// Dumps the context tree to a string.
[[nodiscard]] ss::sstring dump_tree_string(context_ref ctx);

/// \brief Returns a lazy span backtrace for zero-allocation formatting.
/// Walks up the parent chain, producing output like:
/// "grandchild <- child <- root"
/// Use with fmt::format or call format_to() directly.
[[nodiscard]] inline span_backtrace span_backtrace(context_ref ctx) noexcept {
    return context::span_backtrace{ctx};
}

} // namespace context::experimental
