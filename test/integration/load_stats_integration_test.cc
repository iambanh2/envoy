#include "common/config/resources.h"

#include "test/integration/http_integration.h"
#include "test/test_common/network_utility.h"

#include "api/eds.pb.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace {

class LoadStatsIntegrationTest : public HttpIntegrationTest,
                                 public testing::TestWithParam<Network::Address::IpVersion> {
public:
  LoadStatsIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

  void addEndpoint(envoy::api::v2::LocalityLbEndpoints& locality_lb_endpoints, uint32_t index) {
    auto* socket_address = locality_lb_endpoints.add_lb_endpoints()
                               ->mutable_endpoint()
                               ->mutable_address()
                               ->mutable_socket_address();
    socket_address->set_address(Network::Test::getLoopbackAddressString(GetParam()));
    socket_address->set_port_value(service_upstream_[index]->localAddress()->ip()->port());
  }

  // We need to supply the endpoints via EDS to provide locality information for
  // load reporting. Use a filesystem delivery to simplify test mechanics.
  void updateClusterLoadAssignment(const std::vector<uint32_t>& winter_upstreams,
                                   const std::vector<uint32_t>& dragon_upstreeams) {
    envoy::api::v2::ClusterLoadAssignment cluster_load_assignment;
    cluster_load_assignment.set_cluster_name("cluster_0");

    auto* winter = cluster_load_assignment.add_endpoints();
    winter->mutable_locality()->set_region("some_region");
    winter->mutable_locality()->set_zone("zone_name");
    winter->mutable_locality()->set_sub_zone("winter");
    for (uint32_t index : winter_upstreams) {
      addEndpoint(*winter, index);
    }

    auto* dragon = cluster_load_assignment.add_endpoints();
    dragon->mutable_locality()->set_region("some_region");
    dragon->mutable_locality()->set_zone("zone_name");
    dragon->mutable_locality()->set_sub_zone("dragon");
    for (uint32_t index : dragon_upstreeams) {
      addEndpoint(*dragon, index);
    }

    // Write to file the DiscoveryResponse and trigger inotify watch.
    envoy::api::v2::DiscoveryResponse eds_response;
    eds_response.set_version_info(std::to_string(eds_version_++));
    eds_response.set_type_url(Config::TypeUrl::get().ClusterLoadAssignment);
    eds_response.add_resources()->PackFrom(cluster_load_assignment);
    // Past the initial write, need move semantics to trigger inotify move event that the
    // FilesystemSubscriptionImpl is subscribed to.
    if (eds_path_.empty()) {
      eds_path_ =
          TestEnvironment::writeStringToFileForTest("eds.pb_text", eds_response.DebugString());
    } else {
      std::string path = TestEnvironment::writeStringToFileForTest("eds.update.pb_text",
                                                                   eds_response.DebugString());
      ASSERT_EQ(0, ::rename(path.c_str(), eds_path_.c_str()));
    }
  }

  void createUpstreams() override {
    fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_));
    ports_.push_back(fake_upstreams_.back()->localAddress()->ip()->port());
    load_report_upstream_ = fake_upstreams_.back().get();

    for (uint32_t i = 0; i < upstream_endpoints_; ++i) {
      fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP1, version_));
      service_upstream_[i] = fake_upstreams_.back().get();
    }
  }

  void initialize() override {
    updateClusterLoadAssignment({}, {});
    config_helper_.addConfigModifier([this](envoy::api::v2::Bootstrap& bootstrap) {
      // Setup load reporting and corresponding gRPC cluster.
      auto* loadstats_config = bootstrap.mutable_cluster_manager()->mutable_load_stats_config();
      loadstats_config->set_api_type(envoy::api::v2::ApiConfigSource::GRPC);
      loadstats_config->add_cluster_name("load_report");
      auto* load_report_cluster = bootstrap.mutable_static_resources()->add_clusters();
      load_report_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      load_report_cluster->mutable_circuit_breakers()->Clear();
      load_report_cluster->set_name("load_report");
      load_report_cluster->mutable_http2_protocol_options();
      // Put ourselves in a locality that will be used in
      // updateClusterLoadAssignment()
      auto* locality = bootstrap.mutable_node()->mutable_locality();
      locality->set_region("some_region");
      locality->set_zone("zone_name");
      locality->set_sub_zone("winter");
      // Switch predefined cluster_0 to EDS filesystem sourcing.
      auto* cluster_0 = bootstrap.mutable_static_resources()->mutable_clusters(0);
      cluster_0->mutable_hosts()->Clear();
      cluster_0->set_type(envoy::api::v2::Cluster::EDS);
      auto* eds_cluster_config = cluster_0->mutable_eds_cluster_config();
      eds_cluster_config->mutable_eds_config()->set_path(eds_path_);
    });
    named_ports_ = {"http"};
    HttpIntegrationTest::initialize();
  }

  void initiateClientConnection() {
    auto conn = makeClientConnection(lookupPort("http"));
    codec_client_ = makeHttpConnection(std::move(conn));
    Http::TestHeaderMapImpl headers{{":method", "POST"},       {":path", "/test/long/url"},
                                    {":scheme", "http"},       {":authority", "host"},
                                    {"x-lyft-user-id", "123"}, {"x-forwarded-for", "10.0.0.1"}};
    response_.reset(new IntegrationStreamDecoder(*dispatcher_));
    codec_client_->makeRequestWithBody(headers, request_size_, *response_);
  }

  void waitForLoadStatsStream() {
    fake_loadstats_connection_ = load_report_upstream_->waitForHttpConnection(*dispatcher_);
    loadstats_stream_ = fake_loadstats_connection_->waitForNewStream(*dispatcher_);
  }

  void waitForLoadStatsRequest(
      const std::vector<envoy::api::v2::UpstreamLocalityStats>& expected_locality_stats,
      uint64_t dropped = 0) {
    envoy::api::v2::LoadStatsRequest loadstats_request;
    loadstats_stream_->waitForGrpcMessage(*dispatcher_, loadstats_request);
    EXPECT_STREQ("POST", loadstats_stream_->headers().Method()->value().c_str());
    EXPECT_STREQ("/envoy.api.v2.EndpointDiscoveryService/StreamLoadStats",
                 loadstats_stream_->headers().Path()->value().c_str());
    EXPECT_STREQ("application/grpc", loadstats_stream_->headers().ContentType()->value().c_str());

    Protobuf::RepeatedPtrField<envoy::api::v2::ClusterStats> expected_cluster_stats;
    if (!expected_locality_stats.empty()) {
      auto* cluster_stats = expected_cluster_stats.Add();
      cluster_stats->set_cluster_name("cluster_0");
      if (dropped > 0) {
        cluster_stats->set_total_dropped_requests(dropped);
      }
      std::copy(
          expected_locality_stats.begin(), expected_locality_stats.end(),
          Protobuf::RepeatedPtrFieldBackInserter(cluster_stats->mutable_upstream_locality_stats()));
    }
    EXPECT_TRUE(TestUtility::assertRepeatedPtrFieldEqual(expected_cluster_stats,
                                                         loadstats_request.cluster_stats()));
  }

  void waitForUpstreamResponse(uint32_t endpoint_index, uint32_t response_code = 200) {
    fake_upstream_connection_ =
        service_upstream_[endpoint_index]->waitForHttpConnection(*dispatcher_);
    upstream_request_ = fake_upstream_connection_->waitForNewStream(*dispatcher_);
    upstream_request_->waitForEndStream(*dispatcher_);

    upstream_request_->encodeHeaders(
        Http::TestHeaderMapImpl{{":status", std::to_string(response_code)}}, false);
    upstream_request_->encodeData(response_size_, true);
    response_->waitForEndStream();

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_EQ(request_size_, upstream_request_->bodyLength());

    EXPECT_TRUE(response_->complete());
    EXPECT_STREQ(std::to_string(response_code).c_str(),
                 response_->headers().Status()->value().c_str());
    EXPECT_EQ(response_size_, response_->body().size());
  }

  void sendLoadStatsResponse(const std::vector<std::string>& clusters, uint32_t secs) {
    envoy::api::v2::LoadStatsResponse loadstats_response;
    loadstats_response.mutable_load_reporting_interval()->set_seconds(secs);
    for (const auto& cluster : clusters) {
      loadstats_response.add_clusters(cluster);
    }
    loadstats_stream_->sendGrpcMessage(loadstats_response);
  }

  envoy::api::v2::UpstreamLocalityStats localityStats(const std::string& sub_zone, uint64_t success,
                                                      uint64_t error, uint64_t active) {
    envoy::api::v2::UpstreamLocalityStats locality_stats;
    auto* locality = locality_stats.mutable_locality();
    locality->set_region("some_region");
    locality->set_zone("zone_name");
    locality->set_sub_zone(sub_zone);
    locality_stats.set_total_successful_requests(success);
    locality_stats.set_total_error_requests(error);
    locality_stats.set_total_requests_in_progress(active);
    return locality_stats;
  }

  void cleanupUpstreamConnection() {
    codec_client_->close();
    if (fake_upstream_connection_ != nullptr) {
      fake_upstream_connection_->close();
      fake_upstream_connection_->waitForDisconnect();
    }
  }

  void cleanupLoadStatsConnection() {
    if (fake_loadstats_connection_ != nullptr) {
      fake_loadstats_connection_->close();
      fake_loadstats_connection_->waitForDisconnect();
    }
  }

  void sendAndReceiveUpstream(uint32_t endpoint_index, uint32_t response_code = 200) {
    initiateClientConnection();
    waitForUpstreamResponse(endpoint_index, response_code);
    cleanupUpstreamConnection();
  }

  static constexpr uint32_t upstream_endpoints_ = 3;

  FakeHttpConnectionPtr fake_loadstats_connection_;
  FakeStreamPtr loadstats_stream_;
  FakeUpstream* load_report_upstream_{};
  FakeUpstream* service_upstream_[upstream_endpoints_]{};
  std::string eds_path_;
  uint32_t eds_version_{};

  const uint64_t request_size_ = 1024;
  const uint64_t response_size_ = 512;
};

INSTANTIATE_TEST_CASE_P(IpVersions, LoadStatsIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// Validate the load reports for successful requests as cluster membership
// changes.
TEST_P(LoadStatsIntegrationTest, Success) {
  initialize();

  waitForLoadStatsStream();
  waitForLoadStatsRequest({});
  loadstats_stream_->startGrpcStream();

  // Simple 50%/50% split between dragon/winter localities. Also include an
  // unknown cluster to exercise the handling of this case.
  sendLoadStatsResponse({"cluster_0", "cluster_1"}, 1);
  test_server_->waitForCounterGe("load_reporter.requests", 1);

  updateClusterLoadAssignment({0}, {1});
  test_server_->waitForGaugeGe("cluster.cluster_0.membership_total", 2);

  for (uint32_t i = 0; i < 4; ++i) {
    sendAndReceiveUpstream(i % 2);
  }

  waitForLoadStatsRequest({localityStats("winter", 2, 0, 0), localityStats("dragon", 2, 0, 0)});

  EXPECT_EQ(1, test_server_->counter("load_reporter.requests")->value());
  EXPECT_EQ(2, test_server_->counter("load_reporter.responses")->value());
  EXPECT_EQ(0, test_server_->counter("load_reporter.errors")->value());

  // 33%/67% split between dragon/winter localities.
  updateClusterLoadAssignment({0}, {1, 2});
  sendLoadStatsResponse({"cluster_0"}, 1);
  test_server_->waitForGaugeGe("cluster.cluster_0.membership_total", 3);

  for (uint32_t i = 0; i < 6; ++i) {
    sendAndReceiveUpstream((i + 1) % 3);
  }

  waitForLoadStatsRequest({localityStats("winter", 2, 0, 0), localityStats("dragon", 4, 0, 0)});

  EXPECT_EQ(2, test_server_->counter("load_reporter.requests")->value());
  EXPECT_EQ(3, test_server_->counter("load_reporter.responses")->value());
  EXPECT_EQ(0, test_server_->counter("load_reporter.errors")->value());

  // 100% winter locality.
  updateClusterLoadAssignment({}, {});
  updateClusterLoadAssignment({1}, {});
  sendLoadStatsResponse({"cluster_0"}, 1);
  test_server_->waitForCounterGe("load_reporter.requests", 3);

  for (uint32_t i = 0; i < 1; ++i) {
    sendAndReceiveUpstream(1);
  }

  waitForLoadStatsRequest({localityStats("winter", 1, 0, 0)});

  EXPECT_EQ(3, test_server_->counter("load_reporter.requests")->value());
  EXPECT_EQ(4, test_server_->counter("load_reporter.responses")->value());
  EXPECT_EQ(0, test_server_->counter("load_reporter.errors")->value());

  // A LoadStatsResponse arrives before the expiration of the reporting interval.
  sendLoadStatsResponse({"cluster_0"}, 1);
  test_server_->waitForCounterGe("load_reporter.requests", 4);
  sendAndReceiveUpstream(1);
  sendLoadStatsResponse({"cluster_0"}, 1);
  test_server_->waitForCounterGe("load_reporter.requests", 5);
  sendAndReceiveUpstream(1);
  sendAndReceiveUpstream(1);

  waitForLoadStatsRequest({localityStats("winter", 2, 0, 0)});

  EXPECT_EQ(5, test_server_->counter("load_reporter.requests")->value());
  EXPECT_EQ(5, test_server_->counter("load_reporter.responses")->value());
  EXPECT_EQ(0, test_server_->counter("load_reporter.errors")->value());

  cleanupLoadStatsConnection();
}

// Validate the load reports for successful/error requests make sense.
TEST_P(LoadStatsIntegrationTest, Error) {
  initialize();

  waitForLoadStatsStream();
  waitForLoadStatsRequest({});
  loadstats_stream_->startGrpcStream();

  sendLoadStatsResponse({"cluster_0"}, 1);
  test_server_->waitForCounterGe("load_reporter.requests", 1);

  updateClusterLoadAssignment({0}, {});
  test_server_->waitForGaugeGe("cluster.cluster_0.membership_total", 1);

  // This should count as an error since 5xx.
  sendAndReceiveUpstream(0, 503);

  // This should count as "success" since non-5xx.
  sendAndReceiveUpstream(0, 404);

  waitForLoadStatsRequest({localityStats("winter", 1, 1, 0)});

  EXPECT_EQ(1, test_server_->counter("load_reporter.requests")->value());
  EXPECT_EQ(2, test_server_->counter("load_reporter.responses")->value());
  EXPECT_EQ(0, test_server_->counter("load_reporter.errors")->value());

  cleanupLoadStatsConnection();
}

// Validate the load reports for in-progress make sense.
TEST_P(LoadStatsIntegrationTest, InProgress) {
  initialize();

  waitForLoadStatsStream();
  waitForLoadStatsRequest({});
  loadstats_stream_->startGrpcStream();

  sendLoadStatsResponse({"cluster_0"}, 1);
  test_server_->waitForCounterGe("load_reporter.requests", 1);

  updateClusterLoadAssignment({0}, {});
  test_server_->waitForGaugeGe("cluster.cluster_0.membership_total", 1);

  initiateClientConnection();

  waitForLoadStatsRequest({localityStats("winter", 0, 0, 1)});

  waitForUpstreamResponse(0, 503);
  cleanupUpstreamConnection();

  EXPECT_EQ(1, test_server_->counter("load_reporter.requests")->value());
  EXPECT_EQ(2, test_server_->counter("load_reporter.responses")->value());
  EXPECT_EQ(0, test_server_->counter("load_reporter.errors")->value());

  cleanupLoadStatsConnection();
}

// Validate the load reports for dropped requests make sense.
TEST_P(LoadStatsIntegrationTest, Dropped) {
  config_helper_.addConfigModifier([](envoy::api::v2::Bootstrap& bootstrap) {
    auto* cluster_0 = bootstrap.mutable_static_resources()->mutable_clusters(0);
    auto* thresholds = cluster_0->mutable_circuit_breakers()->add_thresholds();
    thresholds->mutable_max_pending_requests()->set_value(0);
  });
  initialize();

  waitForLoadStatsStream();
  waitForLoadStatsRequest({});
  loadstats_stream_->startGrpcStream();

  sendLoadStatsResponse({"cluster_0"}, 1);
  test_server_->waitForCounterGe("load_reporter.requests", 1);

  updateClusterLoadAssignment({0}, {});
  test_server_->waitForGaugeGe("cluster.cluster_0.membership_total", 1);

  // This should count as dropped, since we trigger circuit breaking.
  initiateClientConnection();
  response_->waitForEndStream();
  EXPECT_TRUE(response_->complete());
  EXPECT_STREQ("503", response_->headers().Status()->value().c_str());
  cleanupUpstreamConnection();

  waitForLoadStatsRequest({localityStats("winter", 0, 0, 0)}, 1);

  EXPECT_EQ(1, test_server_->counter("load_reporter.requests")->value());
  EXPECT_EQ(2, test_server_->counter("load_reporter.responses")->value());
  EXPECT_EQ(0, test_server_->counter("load_reporter.errors")->value());

  cleanupLoadStatsConnection();
}

} // namespace
} // namespace Envoy
