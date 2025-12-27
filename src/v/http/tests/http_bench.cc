// Copyright 2024 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "base/seastarx.h"
#include "bytes/iobuf.h"
#include "http/client.h"
#include "net/dns.h"
#include "net/transport.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/routes.hh>
#include <seastar/testing/perf_tests.hh>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>

#include <memory>

using namespace std::chrono_literals;

namespace {

constexpr uint16_t httpd_port_number = 8199;
constexpr const char* httpd_host_name = "127.0.0.1";
constexpr const char* httpd_server_reply = "OK";
constexpr size_t iterations = 100;

struct http_bench_fixture {};

ss::future<size_t> run_get_request_bench() {
    // Setup server
    ::net::unresolved_address server_addr(httpd_host_name, httpd_port_number);
    ::net::base_transport::configuration config{.server_addr = server_addr};

    ss::httpd::http_server_control server;
    co_await server.start();
    co_await server.set_routes([](ss::httpd::routes& r) {
        using namespace ss::httpd;
        auto get_handler = new function_handler(
          [](const_req) -> ss::sstring { return httpd_server_reply; });
        r.add(operation_type::GET, url("/get"), get_handler);
    });

    auto resolved = co_await ::net::resolve_dns(config.server_addr);
    co_await server.listen(resolved);

    // Setup client
    ss::abort_source as;
    ::http::client client(config, as);

    const auto host = std::string_view{config.server_addr.host()};

    // Warmup connection
    {
        ::http::client::request_header header;
        header.method(boost::beast::http::verb::get);
        header.target("/get");
        header.insert(boost::beast::http::field::host, host);

        auto resp = co_await client.request(std::move(header), 5s);
        while (!resp->is_done()) {
            auto buf = co_await resp->recv_some();
            perf_tests::do_not_optimize(buf);
        }
    }

    perf_tests::start_measuring_time();

    for (size_t i = 0; i < iterations; ++i) {
        ::http::client::request_header header;
        header.method(boost::beast::http::verb::get);
        header.target("/get");
        header.insert(boost::beast::http::field::host, host);

        auto resp = co_await client.request(std::move(header), 5s);
        while (!resp->is_done()) {
            auto buf = co_await resp->recv_some();
            perf_tests::do_not_optimize(buf);
        }
    }

    perf_tests::stop_measuring_time();

    co_await client.stop();
    co_await server.stop();
    co_return iterations;
}

} // namespace

PERF_TEST_CN(http_bench_fixture, simple_get_request) {
    co_return co_await run_get_request_bench();
}
