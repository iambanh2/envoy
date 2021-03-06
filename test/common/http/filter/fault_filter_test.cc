#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "envoy/event/dispatcher.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/empty_string.h"
#include "common/config/filter_json.h"
#include "common/http/filter/fault_filter.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/stats/stats_impl.h"

#include "test/common/http/common.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::DoAll;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::WithArgs;
using testing::_;

namespace Envoy {
namespace Http {

class FaultFilterTest : public testing::Test {
public:
  const std::string fixed_delay_and_abort_nodes_json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 100,
        "fixed_duration_ms" : 5000
      },
      "abort" : {
        "abort_percent" : 100,
        "http_status" : 503
      },
      "downstream_nodes": ["canary"]
    }
    )EOF";

  const std::string fixed_delay_only_json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 100,
        "fixed_duration_ms" : 5000
      }
    }
    )EOF";

  const std::string abort_only_json = R"EOF(
    {
      "abort" : {
        "abort_percent" : 100,
        "http_status" : 429
      }
    }
    )EOF";

  const std::string fixed_delay_and_abort_json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 100,
        "fixed_duration_ms" : 5000
      },
      "abort" : {
        "abort_percent" : 100,
        "http_status" : 503
      }
    }
    )EOF";

  const std::string fixed_delay_and_abort_match_headers_json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 100,
        "fixed_duration_ms" : 5000
      },
      "abort" : {
        "abort_percent" : 100,
        "http_status" : 503
      },
      "headers" : [
        {"name" : "X-Foo1", "value" : "Bar"},
        {"name" : "X-Foo2"}
      ]
    }
    )EOF";

  const std::string fault_with_target_cluster_json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 100,
        "fixed_duration_ms" : 5000
      },
      "upstream_cluster" : "www1"
    }
    )EOF";

  void SetUpTest(const std::string json) {
    Json::ObjectSharedPtr config = Json::Factory::loadFromString(json);
    envoy::api::v2::filter::http::HTTPFault fault;

    Config::FilterJson::translateFaultFilter(*config, fault);
    config_.reset(new FaultFilterConfig(fault, runtime_, "prefix.", stats_));
    filter_.reset(new FaultFilter(config_));
    filter_->setDecoderFilterCallbacks(filter_callbacks_);
  }

  void expectDelayTimer(uint64_t duration_ms) {
    timer_ = new Event::MockTimer(&filter_callbacks_.dispatcher_);
    EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(duration_ms)));
    EXPECT_CALL(*timer_, disableTimer());
  }

  FaultFilterConfigSharedPtr config_;
  std::unique_ptr<FaultFilter> filter_;
  NiceMock<MockStreamDecoderFilterCallbacks> filter_callbacks_;
  TestHeaderMapImpl request_headers_;
  Buffer::OwnedImpl data_;
  Stats::IsolatedStoreImpl stats_;
  NiceMock<Runtime::MockLoader> runtime_;
  Event::MockTimer* timer_{};
};

void faultFilterBadConfigHelper(const std::string& json) {
  Json::ObjectSharedPtr config = Json::Factory::loadFromString(json);
  envoy::api::v2::filter::http::HTTPFault fault;
  EXPECT_THROW(Config::FilterJson::translateFaultFilter(*config, fault), EnvoyException);
}

TEST(FaultFilterBadConfigTest, BadAbortPercent) {
  const std::string json = R"EOF(
    {
      "abort" : {
        "abort_percent" : 200,
        "http_status" : 429
      }
    }
  )EOF";

  faultFilterBadConfigHelper(json);
}

TEST(FaultFilterBadConfigTest, EmptyDownstreamNodes) {
  const std::string json = R"EOF(
    {
      "abort" : {
        "abort_percent" : 80,
        "http_status" : 503
      },
      "downstream_nodes": []
    }
  )EOF";

  faultFilterBadConfigHelper(json);
}

TEST(FaultFilterBadConfigTest, MissingHTTPStatus) {
  const std::string json = R"EOF(
    {
      "abort" : {
        "abort_percent" : 100
      }
    }
  )EOF";

  faultFilterBadConfigHelper(json);
}

TEST(FaultFilterBadConfigTest, BadDelayType) {
  const std::string json = R"EOF(
    {
      "delay" : {
        "type" : "foo",
        "fixed_delay_percent" : 50,
        "fixed_duration_ms" : 5000
      }
    }
  )EOF";

  faultFilterBadConfigHelper(json);
}

TEST(FaultFilterBadConfigTest, BadDelayPercent) {
  const std::string json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 500,
        "fixed_duration_ms" : 5000
      }
    }
  )EOF";

  faultFilterBadConfigHelper(json);
}

TEST(FaultFilterBadConfigTest, BadDelayDuration) {
  const std::string json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 50,
        "fixed_duration_ms" : 0
      }
    }
   )EOF";

  faultFilterBadConfigHelper(json);
}

TEST(FaultFilterBadConfigTest, MissingDelayDuration) {
  const std::string json = R"EOF(
    {
      "delay" : {
        "type" : "fixed",
        "fixed_delay_percent" : 50
      }
    }
   )EOF";

  faultFilterBadConfigHelper(json);
}

TEST_F(FaultFilterTest, AbortWithHttpStatus) {
  SetUpTest(abort_only_json);

  // Delay related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 0))
      .WillOnce(Return(false));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected))
      .Times(0);

  // Abort related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", 429))
      .WillOnce(Return(429));

  Http::TestHeaderMapImpl response_headers{
      {":status", "429"}, {"content-length", "18"}, {"content-type", "text/plain"}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(filter_callbacks_, encodeData(_, true));

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected));

  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(0UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(1UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, FixedDelayZeroDuration) {
  SetUpTest(fixed_delay_only_json);

  // Delay related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  // Return 0ms delay
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(0));

  // Abort related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 0))
      .WillOnce(Return(false));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_, setResponseFlag(_)).Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  // Expect filter to continue execution when delay is 0ms
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(0UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, FixedDelayNonZeroDuration) {
  SetUpTest(fixed_delay_only_json);

  // Delay related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(5000UL));

  SCOPED_TRACE("FixedDelayNonZeroDuration");
  expectDelayTimer(5000UL);

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));

  // Abort related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 0))
      .WillOnce(Return(false));

  // Delay only case
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected))
      .Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding());

  EXPECT_EQ(FilterDataStatus::StopIterationAndWatermark, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::StopIteration, filter_->decodeTrailers(request_headers_));
  timer_->callback_();

  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, DelayForDownstreamCluster) {
  SetUpTest(fixed_delay_only_json);

  request_headers_.addCopy("x-envoy-downstream-service-cluster", "cluster");

  // Delay related calls.
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(false));
  EXPECT_CALL(runtime_.snapshot_,
              featureEnabled("fault.http.cluster.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(125UL));
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.cluster.delay.fixed_duration_ms", 125UL))
      .WillOnce(Return(500UL));
  expectDelayTimer(500UL);
  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));

  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));

  // Abort related calls.
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 0))
      .WillOnce(Return(false));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.cluster.abort.abort_percent", 0))
      .WillOnce(Return(false));

  // Delay only case, no aborts.
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.cluster.abort.http_status", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected))
      .Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding());
  EXPECT_EQ(FilterDataStatus::StopIterationAndWatermark, filter_->decodeData(data_, false));

  timer_->callback_();

  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());
  EXPECT_EQ(1UL, stats_.counter("prefix.fault.cluster.delays_injected").value());
  EXPECT_EQ(0UL, stats_.counter("prefix.fault.cluster.aborts_injected").value());
}

TEST_F(FaultFilterTest, FixedDelayAndAbortDownstream) {
  SetUpTest(fixed_delay_and_abort_json);

  request_headers_.addCopy("x-envoy-downstream-service-cluster", "cluster");

  // Delay related calls.
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(false));
  EXPECT_CALL(runtime_.snapshot_,
              featureEnabled("fault.http.cluster.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(125UL));
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.cluster.delay.fixed_duration_ms", 125UL))
      .WillOnce(Return(500UL));
  expectDelayTimer(500UL);

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));

  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));

  // Abort related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 100))
      .WillOnce(Return(false));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.cluster.abort.abort_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", 503))
      .WillOnce(Return(503));
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.cluster.abort.http_status", 503))
      .WillOnce(Return(500));

  Http::TestHeaderMapImpl response_headers{
      {":status", "500"}, {"content-length", "18"}, {"content-type", "text/plain"}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(filter_callbacks_, encodeData(_, true));

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  timer_->callback_();

  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(1UL, config_->stats().aborts_injected_.value());
  EXPECT_EQ(1UL, stats_.counter("prefix.fault.cluster.delays_injected").value());
  EXPECT_EQ(1UL, stats_.counter("prefix.fault.cluster.aborts_injected").value());
}

TEST_F(FaultFilterTest, FixedDelayAndAbort) {
  SetUpTest(fixed_delay_and_abort_json);

  // Delay related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(5000UL));

  SCOPED_TRACE("FixedDelayAndAbort");
  expectDelayTimer(5000UL);

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));

  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));

  // Abort related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", 503))
      .WillOnce(Return(503));

  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "18"}, {"content-type", "text/plain"}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(filter_callbacks_, encodeData(_, true));

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  timer_->callback_();

  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(1UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, FixedDelayAndAbortDownstreamNodes) {
  SetUpTest(fixed_delay_and_abort_nodes_json);

  // Delay related calls.
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(5000UL));

  expectDelayTimer(5000UL);

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));

  request_headers_.addCopy("x-envoy-downstream-service-node", "canary");
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));

  // Abort related calls.
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 100))
      .WillOnce(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", 503))
      .WillOnce(Return(503));

  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "18"}, {"content-type", "text/plain"}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(filter_callbacks_, encodeData(_, true));

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  timer_->callback_();

  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  request_headers_.removeEnvoyDownstreamServiceNode();
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, true));
  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(1UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, FixedDelayAndAbortHeaderMatchSuccess) {
  SetUpTest(fixed_delay_and_abort_match_headers_json);
  request_headers_.addCopy("x-foo1", "Bar");
  request_headers_.addCopy("x-foo2", "RandomValue");

  // Delay related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(5000UL));

  SCOPED_TRACE("FixedDelayAndAbortHeaderMatchSuccess");
  expectDelayTimer(5000UL);

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));

  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));

  // Abort related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", 503))
      .WillOnce(Return(503));

  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "18"}, {"content-type", "text/plain"}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(filter_callbacks_, encodeData(_, true));
  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  timer_->callback_();

  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(1UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, FixedDelayAndAbortHeaderMatchFail) {
  SetUpTest(fixed_delay_and_abort_match_headers_json);
  request_headers_.addCopy("x-foo1", "Bar");
  request_headers_.addCopy("x-foo3", "Baz");

  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", _))
      .Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_, setResponseFlag(_)).Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  // Expect filter to continue execution when headers don't match
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(0UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, TimerResetAfterStreamReset) {
  SetUpTest(fixed_delay_only_json);

  // Prep up with a 5s delay
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(5000UL));

  SCOPED_TRACE("FixedDelayWithStreamReset");
  timer_ = new Event::MockTimer(&filter_callbacks_.dispatcher_);
  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(5000UL)));

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));

  EXPECT_EQ(0UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());

  // delay timer should have been fired by now. If caller resets the stream while we are waiting
  // on the delay timer, check if timers are cancelled
  EXPECT_CALL(*timer_, disableTimer());

  // The timer callback should never be called.
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected))
      .Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());

  EXPECT_EQ(FilterDataStatus::StopIterationAndWatermark, filter_->decodeData(data_, true));

  filter_->onDestroy();
}

TEST_F(FaultFilterTest, FaultWithTargetClusterMatchSuccess) {
  SetUpTest(fault_with_target_cluster_json);
  const std::string upstream_cluster("www1");

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, clusterName())
      .WillOnce(ReturnRef(upstream_cluster));

  // Delay related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", 100))
      .WillOnce(Return(true));

  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", 5000))
      .WillOnce(Return(5000UL));

  SCOPED_TRACE("FaultWithTargetClusterMatchSuccess");
  expectDelayTimer(5000UL);

  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::DelayInjected));
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers_, false));

  // Abort related calls
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", 0))
      .WillOnce(Return(false));

  // Delay only case
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_,
              setResponseFlag(AccessLog::ResponseFlag::FaultInjected))
      .Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding());
  timer_->callback_();

  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(1UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, FaultWithTargetClusterMatchFail) {
  SetUpTest(fault_with_target_cluster_json);
  const std::string upstream_cluster("mismatch");

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, clusterName())
      .WillOnce(ReturnRef(upstream_cluster));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", _))
      .Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_, setResponseFlag(_)).Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(0UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());
}

TEST_F(FaultFilterTest, FaultWithTargetClusterNullRoute) {
  SetUpTest(fault_with_target_cluster_json);
  const std::string upstream_cluster("www1");

  EXPECT_CALL(*filter_callbacks_.route_, routeEntry()).WillOnce(Return(nullptr));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.delay.fixed_delay_percent", _))
      .Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.delay.fixed_duration_ms", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("fault.http.abort.abort_percent", _)).Times(0);
  EXPECT_CALL(runtime_.snapshot_, getInteger("fault.http.abort.http_status", _)).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, _)).Times(0);
  EXPECT_CALL(filter_callbacks_.request_info_, setResponseFlag(_)).Times(0);
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers_));

  EXPECT_EQ(0UL, config_->stats().delays_injected_.value());
  EXPECT_EQ(0UL, config_->stats().aborts_injected_.value());
}

} // namespace Http
} // namespace Envoy
