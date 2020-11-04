#include "test/common/upstream/health_check_fuzz.h"

#include <chrono>
#include <memory>

#include "common/grpc/common.h"

#include "test/common/upstream/utility.h"
#include "test/fuzz/utility.h"

namespace Envoy {
namespace Upstream {
namespace { // gRPC helper methods
// From unit tests
std::vector<std::vector<uint8_t>>
serializeResponseToBufferList(grpc::health::v1::HealthCheckResponse::ServingStatus status,
                              uint64_t chunk_size_from_fuzzer) {
  grpc::health::v1::HealthCheckResponse response;
  response.set_status(status);
  const auto data = Grpc::Common::serializeToGrpcFrame(response);
  uint64_t chunk_size = chunk_size_from_fuzzer % data->length();
  if (chunk_size == 0) {
    ++chunk_size;
  }
  std::vector<std::vector<uint8_t>> bufferList;
  for (size_t i = 0; i < data->length(); i += chunk_size) {
    if (i >= data->length() - chunk_size) {
      // The length of the last chunk
      chunk_size = data->length() - i;
    }
    auto buffer = std::vector<uint8_t>(chunk_size, 0);
    data->copyOut(i, chunk_size, &buffer[0]);
    bufferList.push_back(buffer);
  }
  return bufferList;
}

grpc::health::v1::HealthCheckResponse::ServingStatus
convertToGrpcServingStatus(test::common::upstream::ServingStatus status) {
  switch (status) {
  case test::common::upstream::ServingStatus::UNKNOWN: {
    return grpc::health::v1::HealthCheckResponse::UNKNOWN;
  }
  case test::common::upstream::ServingStatus::SERVING: {
    return grpc::health::v1::HealthCheckResponse::SERVING;
  }
  case test::common::upstream::ServingStatus::NOT_SERVING: {
    return grpc::health::v1::HealthCheckResponse::NOT_SERVING;
  }
  case test::common::upstream::ServingStatus::SERVICE_UNKNOWN: {
    return grpc::health::v1::HealthCheckResponse::SERVICE_UNKNOWN;
  }
  default: // shouldn't hit
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

std::vector<std::vector<uint8_t>>
makeBufferListToRespondWith(test::common::upstream::GrpcRespondBytes grpc_respond_bytes) {
  switch (grpc_respond_bytes.grpc_respond_bytes_selector_case()) {
  case test::common::upstream::GrpcRespondBytes::kStatus: {
    // Structured Response
    grpc::health::v1::HealthCheckResponse::ServingStatus servingStatus =
        convertToGrpcServingStatus(grpc_respond_bytes.status());
    ENVOY_LOG_MISC(trace, "Will respond with a serialized frame with status: {}",
                   grpc_respond_bytes.status());
    return serializeResponseToBufferList(servingStatus,
                                         grpc_respond_bytes.chunk_size_for_structured_response());
  }
  case test::common::upstream::GrpcRespondBytes::kGrpcRespondUnstructuredBytes: {
    std::vector<std::vector<uint8_t>> bufferList;
    // Arbitrarily Generated Bytes
    constexpr auto max_chunks = 128;
    for (int i = 0;
         i <
         std::min(max_chunks, grpc_respond_bytes.grpc_respond_unstructured_bytes().data().size());
         ++i) {
      std::vector<uint8_t> chunk(
          grpc_respond_bytes.grpc_respond_unstructured_bytes().data(i).begin(),
          grpc_respond_bytes.grpc_respond_unstructured_bytes().data(i).end());
      bufferList.push_back(chunk);
    }
    ENVOY_LOG_MISC(trace, "Will respond with arbitrarily generated bytes which have no structure.");
    return bufferList;
  }
  default: // shouldn't hit
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

} // namespace

void HttpHealthCheckFuzz::allocHttpHealthCheckerFromProto(
    const envoy::config::core::v3::HealthCheck& config) {
  health_checker_ = std::make_shared<TestHttpHealthCheckerImpl>(
      *cluster_, config, dispatcher_, runtime_, random_,
      HealthCheckEventLoggerPtr(event_logger_storage_.release()));
  ENVOY_LOG_MISC(trace, "Created Test Http Health Checker");
}

void HttpHealthCheckFuzz::initialize(test::common::upstream::HealthCheckTestCase input) {
  allocHttpHealthCheckerFromProto(input.health_check_config());
  ON_CALL(runtime_.snapshot_, featureEnabled("health_check.verify_cluster", 100))
      .WillByDefault(testing::Return(input.http_verify_cluster()));
  cluster_->prioritySet().getMockHostSet(0)->hosts_ = {
      makeTestHost(cluster_->info_, "tcp://127.0.0.1:80")};
  if (input.upstream_cx_total_inc()) {
    cluster_->info_->stats().upstream_cx_total_.inc();
  }
  expectSessionCreate();
  expectStreamCreate(0);
  // This sets up the possibility of testing hosts that never become healthy
  if (input.start_failed()) {
    cluster_->prioritySet().getMockHostSet(0)->hosts_[0]->healthFlagSet(
        Host::HealthFlag::FAILED_ACTIVE_HC);
  }
  health_checker_->start();
  ON_CALL(runtime_.snapshot_, getInteger("health_check.min_interval", _))
      .WillByDefault(testing::Return(45000));
  // If has an initial jitter, this calls onIntervalBase and finishes startup
  if (DurationUtil::durationToMilliseconds(input.health_check_config().initial_jitter()) != 0) {
    test_sessions_[0]->interval_timer_->invokeCallback();
  }
  reuse_connection_ =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(input.health_check_config(), reuse_connection, true);
}

void HttpHealthCheckFuzz::respond(test::common::upstream::Respond respond, bool last_action) {
  // Timeout timer needs to be explicitly enabled, usually by onIntervalBase() (Callback on interval
  // timer).
  if (!test_sessions_[0]->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping response.");
    return;
  }

  const test::fuzz::Headers& headers = respond.http_respond().headers();
  uint64_t status = respond.http_respond().status();

  std::unique_ptr<Http::TestResponseHeaderMapImpl> response_headers =
      std::make_unique<Http::TestResponseHeaderMapImpl>(
          Fuzz::fromHeaders<Http::TestResponseHeaderMapImpl>(headers, {}, {}));

  response_headers->setStatus(status);

  // Responding with http can cause client to close, if so create a new one.
  bool client_will_close = false;
  if (response_headers->Connection()) {
    client_will_close =
        absl::EqualsIgnoreCase(response_headers->Connection()->value().getStringView(),
                               Http::Headers::get().ConnectionValues.Close);
  } else if (response_headers->ProxyConnection()) {
    client_will_close =
        absl::EqualsIgnoreCase(response_headers->ProxyConnection()->value().getStringView(),
                               Http::Headers::get().ConnectionValues.Close);
  }

  ENVOY_LOG_MISC(trace, "Responded headers {}", *response_headers.get());
  test_sessions_[0]->stream_response_callbacks_->decodeHeaders(std::move(response_headers), true);

  // Interval timer gets turned on from decodeHeaders()
  if ((!reuse_connection_ || client_will_close) && !last_action) {
    ENVOY_LOG_MISC(trace, "Creating client and stream because shouldClose() is true");
    triggerIntervalTimer(true);
  }
}

void HttpHealthCheckFuzz::triggerIntervalTimer(bool expect_client_create) {
  // Interval timer needs to be explicitly enabled, usually by decodeHeaders.
  if (!test_sessions_[0]->interval_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Interval timer is disabled. Skipping trigger interval timer.");
    return;
  }
  if (expect_client_create) {
    expectClientCreate(0);
  }
  expectStreamCreate(0);
  ENVOY_LOG_MISC(trace, "Triggered interval timer");
  test_sessions_[0]->interval_timer_->invokeCallback();
}

void HttpHealthCheckFuzz::triggerTimeoutTimer(bool last_action) {
  // Timeout timer needs to be explicitly enabled, usually by a call to onIntervalBase().
  if (!test_sessions_[0]->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping trigger timeout timer.");
    return;
  }
  ENVOY_LOG_MISC(trace, "Triggered timeout timer");
  test_sessions_[0]->timeout_timer_->invokeCallback(); // This closes the client, turns off timeout
                                                       // and enables interval
  if (!last_action) {
    ENVOY_LOG_MISC(trace, "Creating client and stream from network timeout");
    triggerIntervalTimer(true);
  }
}

void HttpHealthCheckFuzz::raiseEvent(const Network::ConnectionEvent& event_type, bool last_action) {
  test_sessions_[0]->client_connection_->raiseEvent(event_type);
  if (!last_action && event_type != Network::ConnectionEvent::Connected) {
    ENVOY_LOG_MISC(trace, "Creating client and stream from close event");
    triggerIntervalTimer(
        true); // Interval timer is guaranteed to be enabled from a close event - calls
               // onResetStream which handles failure, turning interval timer on and timeout off
  }
}

void TcpHealthCheckFuzz::allocTcpHealthCheckerFromProto(
    const envoy::config::core::v3::HealthCheck& config) {
  health_checker_ = std::make_shared<TcpHealthCheckerImpl>(
      *cluster_, config, dispatcher_, runtime_, random_,
      HealthCheckEventLoggerPtr(event_logger_storage_.release()));
  ENVOY_LOG_MISC(trace, "Created Tcp Health Checker");
}

void TcpHealthCheckFuzz::initialize(test::common::upstream::HealthCheckTestCase input) {
  allocTcpHealthCheckerFromProto(input.health_check_config());
  cluster_->prioritySet().getMockHostSet(0)->hosts_ = {
      makeTestHost(cluster_->info_, "tcp://127.0.0.1:80")};
  expectSessionCreate();
  expectClientCreate();
  health_checker_->start();
  reuse_connection_ =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(input.health_check_config(), reuse_connection, true);
  // The Receive proto message has a validation that if there is a receive field, the text field, a
  // string representing the hex encoded payload has a least one byte.
  if (input.health_check_config().tcp_health_check().receive_size() != 0) {
    ENVOY_LOG_MISC(trace, "Health Checker is only testing to connect");
    empty_response_ = false;
  }
  // Clang tidy throws an error here in regards to a potential leak. It seems to have something to
  // do with shared_ptr and possible cycles in regards to the clusters host objects. Since all this
  // test class directly uses the unit test class that has been in master for a long time, this is
  // likely a false positive.
  if (DurationUtil::durationToMilliseconds(input.health_check_config().initial_jitter()) != 0) {
    interval_timer_->invokeCallback();
  }
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

void TcpHealthCheckFuzz::respond(test::common::upstream::Respond respond, bool last_action) {
  std::string data = respond.tcp_respond().data();
  if (!timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping response.");
    return;
  }
  Buffer::OwnedImpl response;
  response.add(data);

  ENVOY_LOG_MISC(trace, "Responded with {}. Length (in bytes) = {}. This is the string passed in.",
                 data, data.length());
  read_filter_->onData(response, true);

  // The interval timer may not be on. If it's not on, return. An http response will automatically
  // turn on interval and turn off timeout, but for tcp it doesn't if the data doesn't match. If the
  // response doesn't match, it only sets the host to unhealthy. If it does match, it will turn
  // timeout off and interval on.
  if (!reuse_connection_ && interval_timer_->enabled_ && !last_action) {
    triggerIntervalTimer(true);
  }
}

void TcpHealthCheckFuzz::triggerIntervalTimer(bool expect_client_create) {
  if (!interval_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Interval timer is disabled. Skipping trigger interval timer.");
    return;
  }
  if (expect_client_create) {
    ENVOY_LOG_MISC(trace, "Creating client");
    expectClientCreate();
  }
  ENVOY_LOG_MISC(trace, "Triggered interval timer");
  interval_timer_->invokeCallback();
}

void TcpHealthCheckFuzz::triggerTimeoutTimer(bool last_action) {
  if (!timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping trigger timeout timer.");
    return;
  }
  ENVOY_LOG_MISC(trace, "Triggered timeout timer");
  timeout_timer_->invokeCallback(); // This closes the client, turns off timeout
                                    // and enables interval
  if (!last_action) {
    ENVOY_LOG_MISC(trace, "Will create client and stream from network timeout");
    triggerIntervalTimer(true);
  }
}

void TcpHealthCheckFuzz::raiseEvent(const Network::ConnectionEvent& event_type, bool last_action) {
  // On a close event, the health checker will call handleFailure if expect_close_ is false. This is
  // set by multiple code paths. handleFailure() turns on interval and turns off timeout. However,
  // other action of the fuzzer account for this by explicitly invoking a client after
  // expect_close_ gets set to true, turning expect_close_ back to false.
  connection_->raiseEvent(event_type);
  if (!last_action && event_type != Network::ConnectionEvent::Connected) {
    if (!interval_timer_->enabled_) {
      return;
    }
    ENVOY_LOG_MISC(trace, "Will create client from close event");
    triggerIntervalTimer(true);
  }

  // In the specific case of:
  // https://github.com/envoyproxy/envoy/blob/master/source/common/upstream/health_checker_impl.cc#L489
  // This blows away client, should create a new one
  if (event_type == Network::ConnectionEvent::Connected && empty_response_) {
    ENVOY_LOG_MISC(trace, "Will create client from connected event and empty response.");
    triggerIntervalTimer(true);
  }
}

void GrpcHealthCheckFuzz::allocGrpcHealthCheckerFromProto(
    const envoy::config::core::v3::HealthCheck& config) {
  health_checker_ = std::make_shared<TestGrpcHealthCheckerImpl>(
      *cluster_, config, dispatcher_, runtime_, random_,
      HealthCheckEventLoggerPtr(event_logger_storage_.release()));
  ENVOY_LOG_MISC(trace, "Created Test Grpc Health Checker");
}

void GrpcHealthCheckFuzz::initialize(test::common::upstream::HealthCheckTestCase input) {
  test_session_ = std::make_unique<TestSession>();
  allocGrpcHealthCheckerFromProto(input.health_check_config());
  cluster_->prioritySet().getMockHostSet(0)->hosts_ = {
      makeTestHost(cluster_->info_, "tcp://127.0.0.1:80")};
  if (input.upstream_cx_total_inc()) {
    cluster_->info_->stats().upstream_cx_total_.inc();
  }
  expectSessionCreate();
  ON_CALL(dispatcher_, createClientConnection_(_, _, _, _))
      .WillByDefault(testing::InvokeWithoutArgs(
          [&]() -> Network::ClientConnection* { return test_session_->client_connection_; }));

  ON_CALL(*health_checker_, createCodecClient_(_))
      .WillByDefault(
          Invoke([&](Upstream::Host::CreateConnectionData& conn_data) -> Http::CodecClient* {
            TestSession& test_session = *test_session_;
            std::shared_ptr<Upstream::MockClusterInfo> cluster{
                new NiceMock<Upstream::MockClusterInfo>()};
            Event::MockDispatcher dispatcher_;

            test_session.codec_client_ = new CodecClientForTest(
                Http::CodecClient::Type::HTTP1, std::move(conn_data.connection_),
                test_session.codec_, nullptr,
                Upstream::makeTestHost(cluster, "tcp://127.0.0.1:9000"), dispatcher_);
            return test_session.codec_client_;
          }));
  expectStreamCreate();
  health_checker_->start();
  ON_CALL(runtime_.snapshot_, getInteger("health_check.min_interval", _))
      .WillByDefault(testing::Return(45000));

  if (DurationUtil::durationToMilliseconds(input.health_check_config().initial_jitter()) != 0) {
    test_session_->interval_timer_->invokeCallback();
  }

  reuse_connection_ =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(input.health_check_config(), reuse_connection, true);
}

// Logic from respondResponseSpec() in unit tests
void GrpcHealthCheckFuzz::respond(test::common::upstream::Respond respond, bool last_action) {
  const test::common::upstream::GrpcRespond& grpc_respond = respond.grpc_respond();
  if (!test_session_->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping response.");
    return;
  }
  // These booleans help figure out when to end the stream
  const bool has_data = grpc_respond.has_grpc_respond_bytes();
  // Didn't hard code grpc-status to fully explore search space provided by codecs.

  // If the fuzzing engine generates a grpc_respond_trailers message, there is a validation
  // that trailers (test.fuzz.Headers) must be present. If it is present, that means there is
  // trailers that will be passed to decodeTrailers(). An empty trailer map counts as having
  // trailers.
  const bool has_trailers = grpc_respond.has_grpc_respond_trailers();

  ENVOY_LOG_MISC(trace, "Has data: {}. Has trailers: {}.", has_data, has_trailers);

  const bool end_stream_on_headers = !has_data && !has_trailers;

  std::unique_ptr<Http::TestResponseHeaderMapImpl> response_headers =
      std::make_unique<Http::TestResponseHeaderMapImpl>(
          Fuzz::fromHeaders<Http::TestResponseHeaderMapImpl>(
              grpc_respond.grpc_respond_headers().headers(), {}, {}));

  response_headers->setStatus(grpc_respond.grpc_respond_headers().status());

  ENVOY_LOG_MISC(trace, "Responded headers {}", *response_headers.get());
  test_session_->stream_response_callbacks_->decodeHeaders(std::move(response_headers),
                                                           end_stream_on_headers);

  // If the interval timer is enabled, that means that the rpc is complete, as decodeHeaders hit a
  // certain branch that called onRpcComplete(), logically representing a completed rpc call. Thus,
  // skip the next responses until explicitly invoking interval timer as cleanup.
  if (has_data && !test_session_->interval_timer_->enabled_) {
    std::vector<std::vector<uint8_t>> bufferList =
        makeBufferListToRespondWith(grpc_respond.grpc_respond_bytes());
    // If the interval timer is enabled, that means that the rpc is complete, as decodeData hit a
    // certain branch that called onRpcComplete(), logically representing a completed rpc call.
    // Thus, skip the next responses until explicitly invoking interval timer as cleanup.
    for (size_t i = 0; i < bufferList.size() && !test_session_->interval_timer_->enabled_; ++i) {
      const bool end_stream_on_data = !has_trailers && i == bufferList.size() - 1;
      const auto data =
          std::make_unique<Buffer::OwnedImpl>(bufferList[i].data(), bufferList[i].size());
      ENVOY_LOG_MISC(trace, "Responded with data");
      test_session_->stream_response_callbacks_->decodeData(*data, end_stream_on_data);
    }
  }

  // If the interval timer is enabled, that means that the rpc is complete, as decodeData hit a
  // certain branch that called onRpcComplete(), logically representing a completed rpc call. Thus,
  // skip responding with trailers until explicitly invoking interval timer as cleanup.
  if (has_trailers && !test_session_->interval_timer_->enabled_) {
    std::unique_ptr<Http::TestResponseTrailerMapImpl> response_trailers =
        std::make_unique<Http::TestResponseTrailerMapImpl>(
            Fuzz::fromHeaders<Http::TestResponseTrailerMapImpl>(
                grpc_respond.grpc_respond_trailers().trailers(), {}, {}));

    ENVOY_LOG_MISC(trace, "Responded trailers {}", *response_trailers.get());

    test_session_->stream_response_callbacks_->decodeTrailers(std::move(response_trailers));
  }

  // This means that the response did not represent a full rpc response.
  if (!test_session_->interval_timer_->enabled_) {
    return;
  }

  // Once it gets here the health checker will have called onRpcComplete(), logically representing a
  // completed rpc call, which blows away client if reuse connection is set to false or the health
  // checker had a goaway event with no error flag.
  if (!last_action) {
    ENVOY_LOG_MISC(trace, "Triggering interval timer after response");
    triggerIntervalTimer(!reuse_connection_ || received_no_error_goaway_);
    received_no_error_goaway_ = false; // from resetState()
  }
}

void GrpcHealthCheckFuzz::triggerIntervalTimer(bool expect_client_create) {
  if (!test_session_->interval_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Interval timer is disabled. Skipping trigger interval timer.");
    return;
  }
  if (expect_client_create) {
    expectClientCreate();
    ENVOY_LOG_MISC(trace, "Created client");
  }
  expectStreamCreate();
  ENVOY_LOG_MISC(trace, "Created stream");
  test_session_->interval_timer_->invokeCallback();
}

void GrpcHealthCheckFuzz::triggerTimeoutTimer(bool last_action) {
  if (!test_session_->timeout_timer_->enabled_) {
    ENVOY_LOG_MISC(trace, "Timeout timer is disabled. Skipping trigger timeout timer.");
    return;
  }
  ENVOY_LOG_MISC(trace, "Triggered timeout timer");
  test_session_->timeout_timer_->invokeCallback(); // This closes the client, turns off
                                                   // timeout and enables interval

  if ((!reuse_connection_ || received_no_error_goaway_) && !last_action) {
    ENVOY_LOG_MISC(trace, "Triggering interval timer after timeout.");
    triggerIntervalTimer(true);
  } else {
    received_no_error_goaway_ = false; // from resetState()
  }
}

void GrpcHealthCheckFuzz::raiseEvent(const Network::ConnectionEvent& event_type, bool last_action) {
  test_session_->client_connection_->raiseEvent(event_type);
  if (!last_action && event_type != Network::ConnectionEvent::Connected) {
    // Close events will always blow away the client
    ENVOY_LOG_MISC(trace, "Triggering interval timer after close event");
    // Interval timer is guaranteed to be enabled from a close event - calls
    // onResetStream which handles failure, turning interval timer on and timeout off
    triggerIntervalTimer(true);
  }
}

void GrpcHealthCheckFuzz::raiseGoAway(bool no_error) {
  if (no_error) {
    test_session_->codec_client_->raiseGoAway(Http::GoAwayErrorCode::NoError);
    // Will cause other events to blow away client, because this is a "graceful" go away
    received_no_error_goaway_ = true;
  } else {
    // go away events without no error flag explicitly blow away client
    test_session_->codec_client_->raiseGoAway(Http::GoAwayErrorCode::Other);
    triggerIntervalTimer(true);
  }
}

void GrpcHealthCheckFuzz::expectSessionCreate() {
  test_session_->timeout_timer_ = new NiceMock<Event::MockTimer>(&dispatcher_);
  test_session_->interval_timer_ = new NiceMock<Event::MockTimer>(&dispatcher_);
  test_session_->request_encoder_.stream_.callbacks_.clear();
  expectClientCreate();
}

void GrpcHealthCheckFuzz::expectClientCreate() {
  TestSession& test_session = *test_session_;
  test_session.codec_ = new NiceMock<Http::MockClientConnection>();
  test_session.client_connection_ = new NiceMock<Network::MockClientConnection>();
}

void GrpcHealthCheckFuzz::expectStreamCreate() {
  test_session_->request_encoder_.stream_.callbacks_.clear();
  EXPECT_CALL(*test_session_->codec_, newStream(_))
      .WillOnce(DoAll(SaveArgAddress(&test_session_->stream_response_callbacks_),
                      ReturnRef(test_session_->request_encoder_)));
}

Network::ConnectionEvent
HealthCheckFuzz::getEventTypeFromProto(const test::common::upstream::RaiseEvent& event) {
  switch (event) {
  case test::common::upstream::RaiseEvent::CONNECTED: {
    return Network::ConnectionEvent::Connected;
  }
  case test::common::upstream::RaiseEvent::REMOTE_CLOSE: {
    return Network::ConnectionEvent::RemoteClose;
  }
  case test::common::upstream::RaiseEvent::LOCAL_CLOSE: {
    return Network::ConnectionEvent::LocalClose;
  }
  default: // shouldn't hit
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

void HealthCheckFuzz::initializeAndReplay(test::common::upstream::HealthCheckTestCase input) {
  try {
    initialize(input);
  } catch (EnvoyException& e) {
    ENVOY_LOG_MISC(debug, "EnvoyException: {}", e.what());
    return;
  }
  replay(input);
}

void HealthCheckFuzz::replay(const test::common::upstream::HealthCheckTestCase& input) {
  constexpr auto max_actions = 64;
  for (int i = 0; i < std::min(max_actions, input.actions().size()); ++i) {
    const auto& event = input.actions(i);
    // The last_action boolean prevents final actions from creating a client and stream that will
    // never be used.
    const bool last_action = i == std::min(max_actions, input.actions().size()) - 1;
    ENVOY_LOG_MISC(trace, "Action: {}", event.DebugString());
    switch (event.action_selector_case()) {
    case test::common::upstream::Action::kRespond: {
      respond(event.respond(), last_action);
      break;
    }
    case test::common::upstream::Action::kTriggerIntervalTimer: {
      triggerIntervalTimer(false);
      break;
    }
    case test::common::upstream::Action::kTriggerTimeoutTimer: {
      triggerTimeoutTimer(last_action);
      break;
    }
    case test::common::upstream::Action::kRaiseEvent: {
      raiseEvent(getEventTypeFromProto(event.raise_event()), last_action);
      break;
    }
    case test::common::upstream::Action::kRaiseGoAway: {
      raiseGoAway(event.raise_go_away() == test::common::upstream::RaiseGoAway::NO_ERROR);
      break;
    }
    default:
      break;
    }
  }
}

} // namespace Upstream
} // namespace Envoy
