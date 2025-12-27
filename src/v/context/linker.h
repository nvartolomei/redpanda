// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#pragma once

#include "context/context.h"
#include "context/context_frame.h"

#include <list>

namespace context {

/// Mixin for depending on multiple parent contexts, propagating state from any
/// linked parent.
///
/// \note Unlike the zero-allocation core context, this mixin performs heap
/// allocation to store bridge frames that connect to each linked parent.
///
/// \warning All linked parents must outlive the child context, same as the
/// primary parent. This is enforced by assertions in debug builds.
class linker {
    template<typename...>
    friend class context_frame;

    struct bridge_frame final : detail::basic_context_frame {
        bridge_frame(context_ref parent, context::cancel_handle target)
          : basic_context_frame(parent)
          , target_(target) {
            arm_cancel_callback(&cancel_thunk);
        }

        static void cancel_thunk(
          detail::basic_context_frame* base,
          context::cancel_cause cause) noexcept {
            static_cast<bridge_frame*>(base)->target_.trigger(cause);
        }

        context::cancel_handle target_;
    };

public:
    linker() = default;

private:
    template<typename Self, typename... Refs>
    requires(
      sizeof...(Refs) > 0
      && (std::same_as<std::decay_t<Refs>, context_ref> && ...))
    void on_context_init(this Self& self, Refs... extras) noexcept {
        auto handle = self.cancel_handle();
        (
          [&] {
              self.deadline_ = std::min(self.deadline_, extras.deadline());
              // bridge_frame handles already-cancelled case internally
              self.bridges_.emplace_back(extras, handle);
          }(),
          ...);
    }

    std::list<bridge_frame> bridges_;
};

/// Creates a linked context frame.
template<typename... Refs>
requires(
  sizeof...(Refs) >= 1
  && (std::same_as<std::decay_t<Refs>, context_ref> && ...))
[[nodiscard]] auto link(context_ref primary, Refs... additional) {
    return context_frame<linker>{primary, with<linker>(additional...)};
}

} // namespace context
