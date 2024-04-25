// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "base/units.h"
#include "base/vassert.h"
#include "bytes/bytes.h"
#include "bytes/random.h"
#include "config/mock_property.h"
#include "model/fundamental.h"
#include "model/namespace.h"
#include "model/record.h"
#include "model/record_batch_reader.h"
#include "model/record_batch_types.h"
#include "model/tests/random_batch.h"
#include "model/timeout_clock.h"
#include "model/timestamp.h"
#include "random/generators.h"
#include "reflection/adl.h"
#include "storage/batch_cache.h"
#include "storage/log_manager.h"
#include "storage/ntp_config.h"
#include "storage/record_batch_builder.h"
#include "storage/segment_utils.h"
#include "storage/tests/common.h"
#include "storage/tests/disk_log_builder_fixture.h"
#include "storage/tests/storage_test_fixture.h"
#include "storage/tests/utils/disk_log_builder.h"
#include "storage/tests/utils/log_gap_analysis.h"
#include "storage/types.h"
#include "test_utils/async.h"
#include "test_utils/tmp_dir.h"
#include "utils/directory_walker.h"
#include "utils/to_string.h"

#include <seastar/core/future.hh>
#include <seastar/core/io_priority_class.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>

#include <boost/test/tools/old/interface.hpp>
#include <fmt/chrono.h>

#include <algorithm>
#include <exception>
#include <iterator>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <vector>

FIXTURE_TEST(
  test_reading_range_from_a_log_with_write_caching, storage_test_fixture) {
    storage::log_manager mgr = make_log_manager();
    info("Configuration: {}", mgr.config());
    auto ntp = model::ntp("default", "test", 0);
    auto log
      = mgr.manage(storage::ntp_config(ntp, mgr.config().base_dir)).get0();
    auto deferred = ss::defer([&mgr]() mutable { mgr.stop().get0(); });
    auto headers = append_random_batches(
      log,
      200,
      model::term_id(0),
      {},
      storage::log_append_config::fsync::no,
      false);

    log->flush().get();

    // Reclaim everything from cache.
    // storage::testing_details::log_manager_accessor::batch_cache(mgr).clear();

    auto batches = read_and_validate_all_batches(log);
    BOOST_REQUIRE_EQUAL(batches.size(), headers.size());
};
