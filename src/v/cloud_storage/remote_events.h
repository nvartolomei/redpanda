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

#include "absl/container/node_hash_set.h"
#include "base/seastarx.h"
#include "container/intrusive_list_helpers.h"
#include "utils/retry_chain_node.h"

#include <seastar/core/future.hh>

#include <optional>
#include <unordered_set>

namespace cloud_storage {

enum class api_activity_type : uint8_t {
    segment_upload,
    segment_download,
    segment_delete,
    manifest_upload,
    manifest_download,
    controller_snapshot_upload,
    controller_snapshot_download,
    object_upload,
    object_download
};

struct api_activity_notification {
    api_activity_type type;
    bool is_retry;
};

/// Event filter class.
///
/// The filter can be used to subscribe to subset of events.
/// For instance, only to segment downloads and uploads, or to
/// events from all sybsystems except one.
/// The filter is a RAII object. It works until the object
/// exists. If the filter is destroyed before the notification
/// will be received the receiver of the event will see broken
/// promise error.
class event_filter {
    friend class remote;
    friend class remote_service;

public:
    event_filter() = default;

    explicit event_filter(std::unordered_set<api_activity_type> ignored_events)
      : _events_to_ignore(std::move(ignored_events)) {}

    void add_source_to_ignore(const retry_chain_node* source) {
        _sources_to_ignore.insert(source);
    }

    void remove_source_to_ignore(const retry_chain_node* source) {
        _sources_to_ignore.erase(source);
    }

    void cancel() {
        if (_promise.has_value()) {
            _hook.unlink();
            _promise.reset();
        }
    }

private:
    absl::node_hash_set<const retry_chain_node*> _sources_to_ignore;
    std::unordered_set<api_activity_type> _events_to_ignore;
    std::optional<ss::promise<api_activity_notification>> _promise;
    intrusive_list_hook _hook;
};

} // namespace cloud_storage
