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

    /// Trait for detecting linker mixin in context_frame.
    static constexpr bool is_context_linker_mixin = true;

    struct bridge_frame final : detail::basic_context_frame {
        bridge_frame(
          context_ref parent, context::cancel_handle target, context_ref source)
          : basic_context_frame(parent)
          , target_(target)
          , source_(source) {
            if (is_cancelled()) [[unlikely]] {
                on_context_cancel(cancel_cause());
            }
        }

        void on_context_cancel(context::cancel_cause cause) noexcept override {
            target_.trigger(cause);
        }

        /// Returns the source context that created this link.
        [[nodiscard]] const detail::basic_context_frame*
        linked_source() const noexcept override {
            return get_frame(source_);
        }

        context::cancel_handle target_;
        /// Stored as context_ref (not raw pointer) to enable debug ref-counting
        /// assertions that catch use-after-frame-destruction.
        context_ref source_;
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
        context_ref source{*self.parent_};
        (
          [&] {
              self.deadline_ = std::min(self.deadline_, extras.deadline());
              self.bridges_.emplace_back(extras, handle, source);
          }(),
          ...);
    }

    std::list<bridge_frame> bridges_;
};

/// Creates a linked context frame.
template<typename Primary, typename... Refs>
requires(
  std::convertible_to<Primary, context_ref>
  && (std::convertible_to<Refs, context_ref> && ...))
[[nodiscard]] auto link(Primary&& primary, Refs&&... additional) {
    return context_frame<linker>{
      context_ref{std::forward<Primary>(primary)},
      with<linker>(context_ref{std::forward<Refs>(additional)}...)};
}

} // namespace context
