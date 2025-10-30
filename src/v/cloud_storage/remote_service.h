/*
 * Copyright 2025 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#pragma once

#include "cloud_io/remote.h"
#include "cloud_storage/remote_events.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>

#include <base/seastarx.h>

#include <memory>

namespace cloud_storage {

class remote;
class materialized_resources;
class remote_probe;

/// Shard-level service managing `remote` instances.
class remote_service {
    friend class remote;

public:
    explicit remote_service(cloud_io::remote&);

    remote_service(const remote_service&) = delete;
    remote_service& operator=(const remote_service&) = delete;

    remote_service(remote_service&&) = delete;
    remote_service& operator=(remote_service&&) = delete;

    ~remote_service();

public:
    ss::future<> start();
    ss::future<> stop();

public:
    ss::lw_shared_ptr<cloud_storage::remote> remote();

    materialized_resources& materialized() { return *_materialized; }
    remote_probe& probe() { return *_probe; }

    /// Return future that will become available on next cloud storage
    /// api operation.
    ///
    /// \note The operations which are trigger notifications are segment upload,
    /// segment download, segment(s) delete, manifest upload, manifest download.
    /// The notification is generated before the actual use and does not
    /// affected by errors. The notification is generated even if the operation
    /// failed. Also, every retry is generating its own notification.
    ///
    /// \param filter is a notification filter which allows to narrow the set of
    ///        possible notifications by source and type.
    /// \return the future which will be available after the next cloud storage
    ///         API operation.
    ss::future<api_activity_notification> subscribe(event_filter& filter);

private:
    /// Notify all subscribers about segment or manifest upload/download
    void notify_external_subscribers(
      api_activity_notification, const retry_chain_node& caller);
    std::function<void(size_t)>
    make_notify_cb(api_activity_type t, retry_chain_node& retry);

private:
    ss::gate _gate;
    ss::abort_source _as;

    cloud_io::remote& _remote;

    intrusive_list<event_filter, &event_filter::_hook> _filters;

    std::unique_ptr<materialized_resources> _materialized;

    // Lifetime: probe has reference to _materialized, must be destroyed after
    std::unique_ptr<remote_probe> _probe;
};

} // namespace cloud_storage
