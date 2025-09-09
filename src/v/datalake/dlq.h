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

#include <cstdint>
#include <ostream>

namespace datalake {

// Note: Do not forget to register new causes in
// register_invalid_record_metric.
enum class invalid_record_cause : std::uint8_t {
    /// Failed to resolve the Kafka schema for the record. This covers the
    /// cases where the magic byte is missing from the record or schema id
    /// refers to a non-existent schema.
    failed_kafka_schema_resolution,
    /// Failed to translate the record data according to the schema fetched
    /// from the schema registry to an equivalent Iceberg schema/Parquet
    /// format.
    failed_data_translation,
    /// Failed to ensure the table schema matches the inferred Iceberg
    /// schema.
    failed_iceberg_schema_resolution,
};

// Returned string is used as a label for metrics, as a value in the DLQ table.
// Do not change existing values.
constexpr std::string_view to_string_view(invalid_record_cause cause) {
    using enum invalid_record_cause;

    switch (cause) {
    case failed_kafka_schema_resolution:
        return "failed_kafka_schema_resolution";
    case failed_data_translation:
        return "failed_data_translation";
    case failed_iceberg_schema_resolution:
        return "failed_iceberg_schema_resolution";
    }
}

std::ostream& operator<<(std::ostream& os, invalid_record_cause cause);

} // namespace datalake
