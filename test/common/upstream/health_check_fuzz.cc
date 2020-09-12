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
  second_host_ = false;
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
  if (input.create_second_host()) {
    cluster_->prioritySet().getMockHostSet(0)->hosts_.push_back(
        makeTestHost(cluster_->info_, "tcp://127.0.0.1:81"));
    ENVOY_LOG_MISC(trace, "Created second host.");
    second_host_ = true;
    expectSessionCreate();
    expectStreamCreate(1);
    if (input.start_failed()) {
      cluster_->prioritySet().getMockHostSet(0)->hosts_[1]->healthFlagSet(
          Host::HealthFlag::FAILED_ACTIVE_HC);
    }
  }
  health_checker_->start();
  ON_CALL(runtime_.snapshot_, getInteger("health_check.min_interval", _))
      .WillByDefault(testing::Return(45000));
  //If has an initial jitter, this calls onIntervalBase and finishes cleanup
  if (input.health_check_config().initial_jitter().seconds() != 0) {
    test_sessions_[0]->interval_timer_->invokeCallback();
    if (input.create_second_host()) {
      test_sessions_[1]->interval_timer_->invokeCallback();
    }
  }
  replay(input);
}

void HealthCheckFuzz::respondHttp(test::fuzz::Headers headers, absl::string_view status,
                                  bool second_host) {
  const int index = (second_host_ && second_host) ? 1 : 0;

  //Timeout timer needs to be explicity enabled, usually by onIntervalBase() (Callback on interval timer).
  if (!test_sessions_[index]->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping response.");
    return;
  }
  
  std::unique_ptr<Http::TestResponseHeaderMapImpl> response_headers =
      std::make_unique<Http::TestResponseHeaderMapImpl>(
          Fuzz::fromHeaders<Http::TestResponseHeaderMapImpl>(headers, {}, {}));

  response_headers->setStatus(status);

  ENVOY_LOG_MISC(trace, "Responded headers on host {}", index);

  test_sessions_[index]->stream_response_callbacks_->decodeHeaders(std::move(response_headers),
                                                                   true);

  //TODO: This can cause client to crash

}

void HealthCheckFuzz::triggerIntervalTimer(bool second_host) {
  const int index = (second_host_ && second_host) ? 1 : 0;
  //Interval timer needs to be explicitly enabled, usually by decodeHeaders.
  if (!test_sessions_[index]->interval_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Interval timer is disabled. Skipping trigger interval timer.");
    return;
  } //With this check, only crash-2 failed
  //without this check d...also failed, crash-test also failed
  ENVOY_LOG_MISC(trace, "Triggered interval timer on host {}", index);
  expectStreamCreate(index);
  test_sessions_[index]->interval_timer_->invokeCallback();
}

//Note: something wrong with this invokeCallback for interval means the interval timer should be enabled
void HealthCheckFuzz::triggerTimeoutTimer(bool second_host, bool last_action) {
  const int index = (second_host_ && second_host) ? 1 : 0;
  //Timeout timer needs to be explicitly enabled, usually by onIntervalBase()
  if (!test_sessions_[index]->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping trigger timeout timer.");
    return;
  }
  ENVOY_LOG_MISC(trace, "Triggered timeout timer on host {}", index);
  test_sessions_[index]->timeout_timer_->invokeCallback(); //This closes the client, turns off timeout and enables interval
  if (!last_action) {
    ENVOY_LOG_MISC(trace, "Creating client and stream from network timeout on host {}.", index);
    expectClientCreate(index);
    expectStreamCreate(index);
    test_sessions_[index]->interval_timer_->invokeCallback();
    //NOTE: JUST TRYING THIS OUT, if client goes bad for both hosts try this out
    /*if (second_host_) {
      int index2 = 1 - index;
      expectClientCreate(index2);
      expectStreamCreate(index2);
      test_sessions_[index2]->interval_timer_->invokeCallback();
    }*/
  }
}

void HealthCheckFuzz::raiseEvent(test::common::upstream::RaiseEvent event, bool second_host,
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

  const int index = (second_host_ && second_host) ? 1 : 0;
  switch (type_) {
  case HealthCheckFuzz::Type::HTTP: {
    test_sessions_[index]->client_connection_->raiseEvent(eventType);
    if (!last_action && eventType != Network::ConnectionEvent::Connected) {
      ENVOY_LOG_MISC(trace, "Creating client and stream from close event on host {}", index);
      expectClientCreate(index);
      expectStreamCreate(index);
      test_sessions_[index]->interval_timer_->invokeCallback();
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
        respondHttp(event.respond().http_respond().headers(),
                    event.respond().http_respond().status(), event.respond().second_host());
        break;
      }
      // TODO: TCP and gRPC
      default:
        break;
      }
      break;
    }
    case test::common::upstream::Action::kTriggerIntervalTimer: {
      triggerIntervalTimer(event.trigger_interval_timer().second_host());
      break;
    }
    case test::common::upstream::Action::kTriggerTimeoutTimer: {
      triggerTimeoutTimer(event.trigger_timeout_timer().second_host(), last_action);
      break;
    }
    case test::common::upstream::Action::kRaiseEvent: {
      raiseEvent(event.raise_event(), event.raise_event().second_host(), last_action);
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
