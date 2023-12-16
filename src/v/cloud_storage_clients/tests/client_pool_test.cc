/*
 * Copyright 2023 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#include "cloud_storage_clients/client_pool.h"
#include "seastarx.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/defer.hh>

#include <boost/test/tools/interface.hpp>

#include <chrono>
#include <exception>

using namespace std::chrono_literals;

ss::logger test_log("test-log");
static const uint16_t httpd_port_number = 4434;
static constexpr const char* httpd_host_name = "127.0.0.1";

static cloud_storage_clients::s3_configuration transport_configuration() {
    net::unresolved_address server_addr(httpd_host_name, httpd_port_number);
    cloud_storage_clients::s3_configuration conf;
    conf.uri = cloud_storage_clients::access_point_uri(httpd_host_name);
    conf.access_key = cloud_roles::public_key_str("acess-key");
    conf.secret_key = cloud_roles::private_key_str("secret-key");
    conf.region = cloud_roles::aws_region_name("us-east-1");
    conf.server_addr = server_addr;
    conf._probe = ss::make_shared<cloud_storage_clients::client_probe>(
      net::metrics_disabled::yes,
      net::public_metrics_disabled::yes,
      cloud_roles::aws_region_name{"region"},
      cloud_storage_clients::endpoint_url{"endpoint"});
    return conf;
}

SEASTAR_THREAD_TEST_CASE(test_client_pool_acquire_abortable) {
    auto sconf = ss::sharded_parameter([] {
        auto conf = transport_configuration();
        return conf;
    });
    auto conf = transport_configuration();

    ss::sharded<cloud_storage_clients::client_pool> pool;
    size_t num_connections_per_shard = 0;
    pool
      .start(
        num_connections_per_shard,
        sconf,
        cloud_storage_clients::client_pool_overdraft_policy::borrow_if_empty)
      .get();

    pool
      .invoke_on_all([&conf](cloud_storage_clients::client_pool& p) {
          auto cred = cloud_roles::aws_credentials{
            conf.access_key.value(),
            conf.secret_key.value(),
            std::nullopt,
            conf.region};
          p.load_credentials(cred);
      })
      .get();
    auto pool_stop = ss::defer([&pool] { pool.stop().get(); });

    ss::abort_source as;

    bool acquire_succeeded = false;
    auto f = pool.local().acquire(as).then(
      [&acquire_succeeded](auto) -> void { acquire_succeeded = true; });

    ss::sleep(10ms).get();
    BOOST_TEST_REQUIRE(
      !acquire_succeeded, "acquire should be blocked as pool is empty");

    as.request_abort();
    auto acquire_aborted = false;
    try {
        f.get();
    } catch (ss::abort_requested_exception) {
        acquire_aborted = true;
    }

    BOOST_TEST_REQUIRE(acquire_aborted);
}
