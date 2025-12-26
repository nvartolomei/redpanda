// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "context/context.h"

namespace context::detail {

void basic_context_frame::propagate_cancel_to_children() noexcept {
    basic_context_frame* curr = child_;
    while (curr) {
        bool did_cancel = curr->do_cancel_this_frame(cancel_cause_);

        // 1. Dive deeper (Depth First)
        if (curr->child_ && did_cancel) {
            curr = curr->child_;
            continue;
        }

        // 2. Visit siblings or ascend
        while (curr) {
            // If we have a sibling, visit it
            if (curr->next_sibling_) {
                curr = curr->next_sibling_;
                break;
            }

            // No sibling, ascend to parent
            curr = curr->parent_;

            // If we returned to 'this' (the frame being cancelled), we are
            // done
            if (curr == this) {
                return;
            }
        }
    }
}

} // namespace context::detail

namespace context {

system_clock::time_point wall_deadline(const context_ref ctx) noexcept {
    auto internal_deadline = ctx.deadline();

    if (internal_deadline == context::no_deadline) {
        return system_clock::time_point::max();
    }

    // Compute remaining time directly, avoiding time_left()'s redundant
    // no_deadline check and max() clamp (expired deadlines yield past times).
    auto remaining = internal_deadline - clock::now();
    auto sys_tp = lowres_system_clock::now() + remaining;

    return system_clock::time_point{
      std::chrono::duration_cast<system_clock::duration>(
        sys_tp.time_since_epoch())};
}

} // namespace context
