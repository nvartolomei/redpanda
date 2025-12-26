// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "context/tracing.h"

#include "absl/container/inlined_vector.h"

#include <fmt/format.h>

#include <iterator>
#include <ranges>

namespace context {

std::string_view trace_logger::prefix() const {
    auto* span = ctx_.trace_span();
    if (prefix_buf_.size() == 0) {
        auto out = std::back_inserter(prefix_buf_);

        if (!span) {
            fmt::format_to(out, "[no_span] ");
            return {prefix_buf_.data(), prefix_buf_.size()};
        }

        // Collect span IDs (InlinedVector avoids heap for typical depth)
        absl::InlinedVector<uint16_t, 8> ids;
        for (auto* s = span; s; s = s->parent) {
            ids.push_back(s->span.value);
        }

        out = fmt::format_to(out, "[{}", span->trace.value);
        for (unsigned short id : std::ranges::reverse_view(ids)) {
            out = fmt::format_to(out, "~{}", id);
        }
        fmt::format_to(out, " {}] ", span->name.view());
    }
    return {prefix_buf_.data(), prefix_buf_.size()};
}

} // namespace context
