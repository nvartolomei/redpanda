// Copyright 2024 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#pragma once

#include "utils/named_type.h"

#include <cstdint>

namespace experimental::cloud_topics::recovery {

/// Type representing a snapshot version of a partition. A higher version
/// indicates a more recent snapshot.
using ntp_snapshot_version
  = named_type<int64_t, struct ntp_snapshot_version_tag>;

} // namespace experimental::cloud_topics::recovery
