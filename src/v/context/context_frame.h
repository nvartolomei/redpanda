// Copyright 2025 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

/// \file context_frame.h
/// \brief Mixin-composable context frame.
///
/// This header provides `context_frame<Mixins...>` for composing contexts with
/// zero-cost, opt-in features. Most users only need `context/context.h` for
/// `context_ref` and `background()`.
///
/// ## Usage
///
/// ```cpp
/// #include "context/context_frame.h"
/// #include "context/deadline_timer.h"
///
/// using my_frame = context::context_frame<context::deadline_timer>;
/// my_frame frame{parent, context::with<context::deadline_timer>(100ms)};
/// ```
///
/// ## Built-in Mixins
///
/// - `deadline_timer` (deadline_timer.h): Automatic cancellation on timeout.
/// - `abort_source` (abort_source.h): Bridge to Seastar's abort_source.

#pragma once

#include "context/context.h"

#include <tuple>
#include <type_traits>
#include <utility>

namespace context::detail {

/// Wrapper holding constructor args for a specific mixin type.
template<typename Mixin, typename Tuple>
struct mixin_init_wrapper {
    using mixin_type = Mixin;
    Tuple args;
};

/// Extract init args for Mixin from wrappers. Returns empty tuple if not found.
template<typename Mixin>
constexpr std::tuple<> extract_init_args() {
    return {};
}

template<typename Mixin, typename First, typename... Rest>
constexpr auto extract_init_args(First&& first, Rest&&... rest) {
    if constexpr (std::
                    same_as<Mixin, typename std::decay_t<First>::mixin_type>) {
        return std::forward<First>(first).args;
    } else {
        return extract_init_args<Mixin>(std::forward<Rest>(rest)...);
    }
}

/// Check if Mixin is constructible from tuple args (partial specialization).
template<typename Mixin, typename ArgsTuple>
inline constexpr bool constructible_from_tuple_v = false;

template<typename Mixin, typename... Args>
inline constexpr bool constructible_from_tuple_v<Mixin, std::tuple<Args...>>
  = std::is_constructible_v<Mixin, Args...>;

/// Check if Mixin is nothrow constructible from tuple args.
template<typename Mixin, typename ArgsTuple>
inline constexpr bool nothrow_constructible_from_tuple_v = false;

template<typename Mixin, typename... Args>
inline constexpr bool
  nothrow_constructible_from_tuple_v<Mixin, std::tuple<Args...>>
  = std::is_nothrow_constructible_v<Mixin, Args...>;

/// Check if construct_mixin would be noexcept for given Mixin and ArgsTuple.
template<typename Mixin, typename ArgsTuple>
inline constexpr bool construct_mixin_nothrow_v = [] {
    using Args = std::decay_t<ArgsTuple>;
    if constexpr (constructible_from_tuple_v<Mixin, Args>) {
        return nothrow_constructible_from_tuple_v<Mixin, Args>;
    } else {
        return std::is_nothrow_default_constructible_v<Mixin>;
    }
}();

/// Construct mixin: use matching constructor if available, else default.
template<typename Mixin, typename ArgsTuple>
constexpr Mixin construct_mixin(ArgsTuple&& args) noexcept(
  construct_mixin_nothrow_v<Mixin, ArgsTuple>) {
    if constexpr (constructible_from_tuple_v<Mixin, std::decay_t<ArgsTuple>>) {
        return std::make_from_tuple<Mixin>(std::forward<ArgsTuple>(args));
    } else {
        return Mixin{};
    }
}

/// Check if Init's mixin_type is one of Mixins...
template<typename Init, typename... Mixins>
inline constexpr bool targets_one_of_v
  = (std::is_same_v<typename std::decay_t<Init>::mixin_type, Mixins> || ...);

/// Count how many Inits target Mixin.
template<typename Mixin, typename... Inits>
inline constexpr size_t wrapper_count_v
  = ((std::is_same_v<typename std::decay_t<Inits>::mixin_type, Mixin> ? 1 : 0) + ...);

} // namespace context::detail

namespace context {

/// Creates an init wrapper for passing constructor arguments to a mixin.
/// Usage: context::with<MyMixin>(arg1, arg2, ...)
template<typename Mixin, typename... Args>
[[nodiscard]] constexpr auto with(Args&&... args) {
    return detail::mixin_init_wrapper<Mixin, std::tuple<std::decay_t<Args>...>>{
      std::tuple<std::decay_t<Args>...>{std::forward<Args>(args)...}};
}

template<typename... Mixins>
class context_frame final
  : public detail::basic_context_frame
  , public Mixins... {
public:
    /// Construct with parent only (default-init all mixins).
    explicit context_frame(context_ref parent) noexcept(
      ((std::is_nothrow_default_constructible_v<Mixins>
        && init_mixin_nothrow<Mixins, std::tuple<>>())
       && ...))
      : detail::basic_context_frame(parent)
      , Mixins()... {
        (init_mixin<Mixins>(std::tuple<>{}), ...);

        if (this->is_cancelled()) {
            on_context_cancel(this->cancel_cause());
        }
    }

    /// Construct with parent and mixin init wrappers.
    /// Usage: context_frame(parent, context::with<MixinA>(args...),
    ///                      context::with<MixinB>(args...))
    template<class... Inits>
    requires(sizeof...(Inits) > 0
             && (detail::targets_one_of_v<Inits, Mixins...> && ...)
             && ((detail::wrapper_count_v<Mixins, Inits...> <= 1) && ...))
    explicit context_frame(context_ref parent, Inits&&... inits) noexcept(
      ((detail::construct_mixin_nothrow_v<
          Mixins,
          decltype(detail::extract_init_args<Mixins>(std::declval<Inits>()...))>
        && init_mixin_nothrow<
          Mixins,
          decltype(detail::extract_init_args<Mixins>(
            std::declval<Inits>()...))>())
       && ...))
      : detail::basic_context_frame(parent)
      , Mixins(
          detail::construct_mixin<Mixins>(detail::extract_init_args<Mixins>(
            std::forward<Inits>(inits)...)))... {
        (init_mixin<Mixins>(
           detail::extract_init_args<Mixins>(std::forward<Inits>(inits)...)),
         ...);

        if (this->is_cancelled()) {
            on_context_cancel(this->cancel_cause());
        }
    }

private:
    /// Check if on_context_init(args...) is noexcept for given Mixin.
    template<typename Mixin, typename Args, size_t... Is>
    static constexpr bool init_mixin_nothrow_impl(std::index_sequence<Is...>) {
        if constexpr (!detail::constructible_from_tuple_v<Mixin, Args>) {
            return noexcept(
              std::declval<context_frame&>().Mixin::on_context_init(
                std::get<Is>(std::declval<Args>())...));
        }
        return true;
    }

    /// Check if init_mixin would be noexcept for given Mixin and ArgsTuple.
    template<typename Mixin, typename ArgsTuple>
    static constexpr bool init_mixin_nothrow() {
        using Args = std::decay_t<ArgsTuple>;
        if constexpr (std::tuple_size_v<Args> > 0) {
            return init_mixin_nothrow_impl<Mixin, Args>(
              std::make_index_sequence<std::tuple_size_v<Args>>{});
        } else {
            if constexpr (requires(context_frame& f) {
                              f.Mixin::on_context_init();
                          }) {
                return noexcept(
                  std::declval<context_frame&>().Mixin::on_context_init());
            }
            return true;
        }
    }

    /// Call on_context_init hook for a mixin based on its init args.
    template<typename Mixin, typename ArgsTuple>
    void init_mixin(ArgsTuple&& args) noexcept(
      init_mixin_nothrow<Mixin, ArgsTuple>()) {
        using Args = std::decay_t<ArgsTuple>;
        if constexpr (std::tuple_size_v<Args> > 0) {
            // Has args: call on_context_init(args...) only if no matching ctor
            if constexpr (!detail::constructible_from_tuple_v<Mixin, Args>) {
                std::apply(
                  [this](auto&&... a) {
                      this->Mixin::on_context_init(
                        std::forward<decltype(a)>(a)...);
                  },
                  std::forward<ArgsTuple>(args));
            }
        } else {
            // No args: call on_context_init() if it exists
            if constexpr (requires { this->Mixin::on_context_init(); }) {
                this->Mixin::on_context_init();
            }
        }
    }

protected:
    void on_context_cancel(const context::cancel_cause cause) noexcept final {
        (..., [&] {
            constexpr bool has_hook = requires {
                this->Mixins::on_context_cancel(cause);
            };
            constexpr bool expects_hook = requires {
                typename Mixins::cancellable;
            };

            // SAFETY ASSERTION:
            // If you promised a hook (expects_hook), but we can't call it
            // (!has_hook), it means there is a typo or a missing 'friend'
            // declaration.
            static_assert(
              !expects_hook || has_hook,
              "Mixin defined 'cancellable' but "
              "'on_context_cancel' is missing or private/unfriendly!");

            if constexpr (has_hook) {
                this->Mixins::on_context_cancel(cause);
            }
        }());
    }
};

} // namespace context
