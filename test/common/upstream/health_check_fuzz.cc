#include "test/common/upstream/health_check_fuzz.h"

#include <chrono>

#include "test/common/upstream/utility.h"
#include "test/fuzz/utility.h"

namespace Envoy {
namespace Upstream {

void HealthCheckFuzz::allocHealthCheckerFromProto(
    const envoy::config::core::v3::HealthCheck& config) {
  health_checker_ = std::make_shared<TestHttpHealthCheckerImpl>(
      *cluster_, config, dispatcher_, runtime_, random_,
      HealthCheckEventLoggerPtr(event_logger_storage_.release()));
  ENVOY_LOG_MISC(trace, "Created Test Health Checker");
}

void HealthCheckFuzz::initializeAndReplay(test::common::upstream::HealthCheckTestCase input) {
  try {
    allocHealthCheckerFromProto(input.health_check_config());
  } catch (EnvoyException& e) {
    ENVOY_LOG_MISC(debug, "EnvoyException: {}", e.what());
    return;
  }
  ON_CALL(runtime_.snapshot_, featureEnabled("health_check.verify_cluster", 100))
      .WillByDefault(testing::Return(input.http_verify_cluster()));
  cluster_->prioritySet().getMockHostSet(0)->hosts_ = {
      makeTestHost(cluster_->info_, "tcp://127.0.0.1:80")};
  expectSessionCreate();
  expectStreamCreate(0);
  if (input.start_failed()) {
    cluster_->prioritySet().getMockHostSet(0)->hosts_[0]->healthFlagSet(
        Host::HealthFlag::FAILED_ACTIVE_HC);
  }
  health_checker_->start();
  ON_CALL(runtime_.snapshot_, getInteger("health_check.min_interval", _))
      .WillByDefault(testing::Return(45000));
  //If has an initial jitter, this calls onIntervalBase and finishes cleanup
  if (input.health_check_config().initial_jitter().seconds() != 0) {
    test_sessions_[0]->interval_timer_->invokeCallback();
  }
  reuse_connection_ = true;
  if (input.health_check_config().has_reuse_connection()) { //TODO: Does this make sense?
    reuse_connection_ = input.health_check_config().reuse_connection().value();
  }
  replay(input);
}

void HealthCheckFuzz::respondHttp(test::fuzz::Headers headers, absl::string_view status) {

  //Timeout timer needs to be explicity enabled, usually by onIntervalBase() (Callback on interval timer).
  if (!test_sessions_[0]->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping response.");
    return;
  }
  
  std::unique_ptr<Http::TestResponseHeaderMapImpl> response_headers =
      std::make_unique<Http::TestResponseHeaderMapImpl>(
          Fuzz::fromHeaders<Http::TestResponseHeaderMapImpl>(headers, {}, {}));

  response_headers->setStatus(status);

  ENVOY_LOG_MISC(trace, "Responded headers");

  //TODO: This can cause client to close, if so create a new one
  bool client_will_close = false;
  if (response_headers->Connection()) {
    client_will_close =
        absl::EqualsIgnoreCase(response_headers->Connection()->value().getStringView(),
                               Http::Headers::get().ConnectionValues.Close);
  }

  if (response_headers->ProxyConnection()) {
    client_will_close =
        absl::EqualsIgnoreCase(response_headers->ProxyConnection()->value().getStringView(),
                               Http::Headers::get().ConnectionValues.Close);
  }

  test_sessions_[0]->stream_response_callbacks_->decodeHeaders(std::move(response_headers),
                                                                   true);

  if (!reuse_connection_ || client_will_close) {
    ENVOY_LOG_MISC(trace, "Creating client and stream because shouldClose() is true");
    expectClientCreate(0);
    expectStreamCreate(0);
    test_sessions_[0]->interval_timer_->invokeCallback();
  }

}

void HealthCheckFuzz::triggerIntervalTimer() {
  //Interval timer needs to be explicitly enabled, usually by decodeHeaders.
  if (!test_sessions_[0]->interval_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Interval timer is disabled. Skipping trigger interval timer.");
    return;
  }
  if (test_sessions_[0]->codec_client_destructed_) {
    expectClientCreate(0);
    test_sessions_[0]->codec_client_destructed_ = false;
    ENVOY_LOG_MISC(trace, "CodecClient was destructed. Expecting a new one to be made.");
  }
  expectStreamCreate(0);
  ENVOY_LOG_MISC(trace, "Triggered interval timer");
  test_sessions_[0]->interval_timer_->invokeCallback();
}

//Note: something wrong with this invokeCallback for interval means the interval timer should be enabled
void HealthCheckFuzz::triggerTimeoutTimer(bool last_action) {
  //Timeout timer needs to be explicitly enabled, usually by onIntervalBase()
  if (!test_sessions_[0]->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping trigger timeout timer.");
    return;
  }
  ENVOY_LOG_MISC(trace, "Triggered timeout timer");
  test_sessions_[0]->timeout_timer_->invokeCallback(); //This closes the client, turns off timeout and enables interval
  if (!last_action) {
    ENVOY_LOG_MISC(trace, "Creating client and stream from network timeout");
    expectClientCreate(0);
    expectStreamCreate(0);
    test_sessions_[0]->interval_timer_->invokeCallback();
  }
}

void HealthCheckFuzz::raiseEvent(test::common::upstream::RaiseEvent event,
                                 bool last_action) {
  Network::ConnectionEvent eventType;
  switch (event.event_selector_case()) {
  case test::common::upstream::RaiseEvent::kConnected: {
    eventType = Network::ConnectionEvent::Connected;
    break;
  }
  case test::common::upstream::RaiseEvent::kRemoteClose: {
    eventType = Network::ConnectionEvent::RemoteClose;
    break;
  }
  case test::common::upstream::RaiseEvent::kLocalClose: {
    eventType = Network::ConnectionEvent::LocalClose;
    break;
  }
  default: // shouldn't hit
    eventType = Network::ConnectionEvent::Connected;
    break;
  }

  switch (type_) {
  case HealthCheckFuzz::Type::HTTP: {
    test_sessions_[0]->client_connection_->raiseEvent(eventType);
    if (!last_action && eventType != Network::ConnectionEvent::Connected) { //TODO: Discuss with Asra, you can either have this hardcoded here or handled in an expect stream create, but I feel like hardcoding would be better in terms of recreating client/stream, as otherwise events would have to cycle until invokeIntervalTimer()  to do anything
      ENVOY_LOG_MISC(trace, "Creating client and stream from close event");
      expectClientCreate(0);
      expectStreamCreate(0);
      test_sessions_[0]->interval_timer_->invokeCallback();
    }
    break;
  }
  default:
    break;
  }
}

void HealthCheckFuzz::replay(test::common::upstream::HealthCheckTestCase input) {
  for (int i = 0; i < input.actions().size(); ++i) {
    const auto& event = input.actions(i);
    bool last_action = i == input.actions().size() - 1;
    ENVOY_LOG_MISC(trace, "Action: {}", event.DebugString());
    switch (event.action_selector_case()) { // TODO: Once added implementations for tcp and gRPC,
                                            // move this to a separate method, handleHttp
    case test::common::upstream::Action::kRespond: {
      switch (type_) {
      case HealthCheckFuzz::Type::HTTP: {
        if (event.respond().http_respond().status().empty()) { //TODO: Required because can't documentation about requireds for strings in proto.
          return;
        }
        respondHttp(event.respond().http_respond().headers(),
                    event.respond().http_respond().status());
        break;
      }
      // TODO: TCP and gRPC
      default:
        break;
      }
      break;
    }
    case test::common::upstream::Action::kTriggerIntervalTimer: {
      triggerIntervalTimer();
      break;
    }
    case test::common::upstream::Action::kTriggerTimeoutTimer: {
      triggerTimeoutTimer(last_action);
      break;
    }
    case test::common::upstream::Action::kRaiseEvent: {
      raiseEvent(event.raise_event(), last_action);
      break;
    }
    default:
      break;
    }
  }
  //TODO: Cleanup?
}

} // namespace Upstream
} // namespace Envoy
