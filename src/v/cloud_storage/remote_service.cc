/*
 * Copyright 2025 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#include "cloud_storage/remote_service.h"

#include "cloud_storage/logger.h"
#include "cloud_storage/materialized_resources.h"
#include "cloud_storage/remote_probe.h"
#include "config/configuration.h"

namespace cloud_storage {

remote_service::remote_service(cloud_io::remote& remote)
  : _remote(remote)
  , _materialized(std::make_unique<materialized_resources>())
  , _probe(
      std::make_unique<remote_probe>(
        remote_metrics_disabled(config::shard_local_cfg().disable_metrics()),
        remote_metrics_disabled(
          config::shard_local_cfg().disable_public_metrics()),
        *_materialized)) {}

remote_service::~remote_service() = default;

ss::future<> remote_service::start() { co_await _materialized->start(); }

ss::future<> remote_service::stop() {
    cst_log.debug("Stopping remote_service...");
    _as.request_abort();
    auto gate_close = _gate.close();
    co_await _materialized->stop();
    co_await std::move(gate_close);
    cst_log.debug("Stopped remote_service...");
}

ss::lw_shared_ptr<cloud_storage::remote> remote_service::remote() {
    return ss::make_lw_shared<cloud_storage::remote>(*this, _remote);
}

ss::future<api_activity_notification>
remote_service::subscribe(event_filter& filter) {
    _as.check();
    auto holder = _gate.hold();
    vassert(filter._hook.is_linked() == false, "Filter is already in use");
    _filters.push_back(filter);
    filter._promise.emplace();
    return filter._promise->get_future().then(
      [h = std::move(holder)](api_activity_notification r) { return r; });
    ;
}

void remote_service::notify_external_subscribers(
  api_activity_notification event, const retry_chain_node& caller) {
    const auto* caller_root = caller.get_root();

    for (auto& flt : _filters) {
        if (flt._events_to_ignore.contains(event.type)) {
            continue;
        }

        if (flt._sources_to_ignore.contains(caller_root)) {
            continue;
        }

        // Invariant: the filter._promise is always initialized
        // by the 'subscribe' method.
        vassert(
          flt._promise.has_value(),
          "Filter object is not initialized properly");
        flt._promise->set_value(event);
        flt._promise = std::nullopt;
        // NOTE: the filter object can be reused by the owner
    }

    _filters.remove_if(
      [](const event_filter& f) { return !f._promise.has_value(); });
}

std::function<void(size_t)>
remote_service::make_notify_cb(api_activity_type t, retry_chain_node& retry) {
    return [this, t, &retry](size_t attempt_num) {
        notify_external_subscribers(
          api_activity_notification{.type = t, .is_retry = attempt_num > 1},
          retry);
    };
}

} // namespace cloud_storage
