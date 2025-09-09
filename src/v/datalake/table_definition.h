/*
 * Copyright 2024 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */
#pragma once

#include "iceberg/datatypes.h"

#include <string_view>

namespace datalake {

// Definitions for default table metadata.

// Contains some minimal fields used for all tables, even those with no schemas.
// TODO: rename to redpanda_fields_struct_type?
iceberg::struct_type schemaless_struct_type();
inline constexpr std::string_view rp_struct_name = "redpanda";

} // namespace datalake
