// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "common/common/enum_to_int.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"
#include "common/tracing/http_tracer_impl.h"
#include "envoy/event/deferred_deletable.h"
#include "src/envoy/http/service_control/http_call.h"

using ::google::api::envoy::http::common::HttpUri;
using ::google::protobuf::util::Status;
using ::google::protobuf::util::error::Code;
namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ServiceControl {
namespace {

constexpr absl::string_view KApplicationProto = "application/x-protobuf";

class HttpCallImpl : public HttpCall,
                     public Event::DeferredDeletable,
                     public Logger::Loggable<Logger::Id::filter>,
                     public Http::AsyncClient::Callbacks {
 public:
  HttpCallImpl(Upstream::ClusterManager& cm, Event::Dispatcher& dispatcher,
               const HttpUri& uri, const std::string& suffix_url,
               std::function<const std::string&()> token_fn,
               const Protobuf::Message& body, uint32_t timeout_ms,
               uint32_t retries, Envoy::Tracing::Span& parent_span,
               Envoy::TimeSource& time_source,
               const std::string& trace_operation_name,
               HttpCall::DoneFunc on_done)
      : cm_(cm),
        dispatcher_(dispatcher),
        http_uri_(uri),
        retries_(retries),
        request_count_(0),
        timeout_ms_(timeout_ms),
        token_fn_(token_fn),
        parent_span_(parent_span),
        time_source_(time_source),
        trace_operation_name_(trace_operation_name) {
    uri_ = http_uri_.uri() + suffix_url;

    Http::Utility::extractHostPathFromUri(uri_, host_, path_);
    body.SerializeToString(&str_body_);

    ASSERT(!on_done_);
    on_done_ = on_done;
    ENVOY_LOG(trace, "{}", __func__);
  }

  ~HttpCallImpl() {}

  void call() override { makeOneCall(); }

  // HTTP async receive methods
  void onSuccess(Http::MessagePtr&& response) override {
    ENVOY_LOG(trace, "{}", __func__);
    const uint64_t status_code =
        Http::Utility::getResponseStatus(response->headers());

    request_span_->setTag(Tracing::Tags::get().HttpStatusCode,
                          std::to_string(status_code));
    request_span_->finishSpan();

    std::string body;
    if (response->body()) {
      const auto len = response->body()->length();
      body = std::string(static_cast<char*>(response->body()->linearize(len)),
                         len);
    }
    if (status_code == enumToInt(Http::Code::OK)) {
      ENVOY_LOG(debug, "http call [uri = {}]: success with body {}", uri_,
                body);
      on_done_(Status::OK, body);
    } else {
      if (attemptRetry(status_code)) {
        return;
      }

      ENVOY_LOG(debug, "http call response status code: {}, body: {}",
                status_code, body);
      on_done_(Status(Code::INTERNAL, "Failed to call service control"), body);
    }
    reset();
    deferredDelete();
  }

  void onFailure(Http::AsyncClient::FailureReason reason) override {
    // The status code in reason is always 0.
    ENVOY_LOG(debug, "http call network error");

    switch (reason) {
      case Http::AsyncClient::FailureReason::Reset:
        request_span_->setTag(Tracing::Tags::get().Error,
                              "the stream has been reset");
        break;
      default:
        request_span_->setTag(Tracing::Tags::get().Error,
                              "unknown network error");
        break;
    }
    request_span_->finishSpan();

    if (attemptRetry(0)) {
      return;
    }

    on_done_(Status(Code::INTERNAL, "Failed to call service control"),
             std::string());
    reset();
    deferredDelete();
  }

 private:
  bool attemptRetry(const uint64_t& status_code) {
    // skip if it is the client side problem.
    if (status_code >= 400 && status_code < 500) {
      return false;
    }
    if (retries_ <= 0) {
      return false;
    }
    retries_--;
    ENVOY_LOG(debug,
              "after {} times failures, retrying http call [uri = {}], with "
              "{} remaining chances",
              request_count_, uri_, retries_);

    reset();
    makeOneCall();
    return true;
  }

  void makeOneCall() {
    request_count_++;
    std::string token = token_fn_();
    if (token.empty()) {
      on_done_(Status(Code::INTERNAL,
                      "Missing access token for service control call"),
               "");
      return;
    }

    // Trace the request
    auto span_name = request_count_ == 1
                         ? trace_operation_name_
                         : absl::StrCat(trace_operation_name_, " - Retry ",
                                        request_count_ - 1);
    request_span_ =
        parent_span_.spawnChild(Envoy::Tracing::EgressConfig::get(), span_name,
                                time_source_.systemTime());
    request_span_->setTag(Tracing::Tags::get().Component,
                          Tracing::Tags::get().Proxy);
    request_span_->setTag(Tracing::Tags::get().UpstreamCluster,
                          http_uri_.cluster());
    request_span_->setTag(Tracing::Tags::get().HttpUrl, uri_);
    request_span_->setTag(Tracing::Tags::get().HttpMethod, "POST");

    Http::MessagePtr message = prepareHeaders(token);
    ENVOY_LOG(debug, "http call from [uri = {}]: start", uri_);
    request_ = cm_.httpAsyncClientForCluster(http_uri_.cluster())
                   .send(std::move(message), *this,
                         Http::AsyncClient::RequestOptions().setTimeout(
                             std::chrono::milliseconds(timeout_ms_)));
  }

  void cancel() override {
    if (request_span_) {
      request_span_->setTag(Tracing::Tags::get().Error,
                            Tracing::Tags::get().Canceled);
      request_span_->finishSpan();
    }

    if (request_) {
      request_->cancel();
      ENVOY_LOG(debug, "Http call [uri = {}]: canceled", uri_);
      reset();
    }
    deferredDelete();
  }

  void reset() { request_ = nullptr; }

  Http::MessagePtr prepareHeaders(const std::string& token) {
    Http::MessagePtr message(new Http::RequestMessageImpl());
    message->headers().insertPath().value(path_.data(), path_.size());
    message->headers().insertHost().value(host_.data(), host_.size());

    message->headers().insertMethod().value().setReference(
        Http::Headers::get().MethodValues.Post);

    message->body() =
        std::make_unique<Buffer::OwnedImpl>(str_body_.data(), str_body_.size());
    message->headers().insertContentLength().value(message->body()->length());

    // assume token is not empty
    std::string token_value = "Bearer " + token;
    message->headers().insertAuthorization().value(token_value.data(),
                                                   token_value.size());
    message->headers().insertContentType().value(KApplicationProto.data(),
                                                 KApplicationProto.size());
    return message;
  }

  void deferredDelete() {
    dispatcher_.deferredDelete(std::unique_ptr<HttpCallImpl>(this));
  }

 private:
  // The upstream cluster manager
  Upstream::ClusterManager& cm_;
  // The dispatcher for this thread
  Event::Dispatcher& dispatcher_;

  // The request
  Http::AsyncClient::Request* request_{};

  // The callback function when request finished
  HttpCall::DoneFunc on_done_;

  // The serialized request body
  std::string str_body_;

  // The request uri
  std::string uri_;
  // The host of the request uri
  const HttpUri& http_uri_;
  // The host of the request uri with buffer owned by uri_
  absl::string_view host_;
  // The path of the request uri with buffer owned by uri_
  absl::string_view path_;

  // The remaining retry times
  uint32_t retries_;
  // The sent request count
  uint32_t request_count_;
  // The timeout
  uint32_t timeout_ms_;

  // The function for getting token
  std::function<const std::string&()> token_fn_;

  // Tracing data
  Envoy::Tracing::Span& parent_span_;
  Envoy::TimeSource& time_source_;
  Envoy::Tracing::SpanPtr request_span_;
  const std::string trace_operation_name_;
};

}  // namespace

HttpCall* HttpCall::create(
    Upstream::ClusterManager& cm, Event::Dispatcher& dispatcher,
    const ::google::api::envoy::http::common::HttpUri& uri,
    const std::string& suffix_url, std::function<const std::string&()> token_fn,
    const Protobuf::Message& body, uint32_t timeout_ms, uint32_t retries,
    Envoy::Tracing::Span& parent_span, Envoy::TimeSource& time_source,
    const std::string& trace_operation_name, HttpCall::DoneFunc on_done) {
  return new HttpCallImpl(cm, dispatcher, uri, suffix_url, token_fn, body,
                          timeout_ms, retries, parent_span, time_source,
                          trace_operation_name, on_done);
}

}  // namespace ServiceControl
}  // namespace HttpFilters
}  // namespace Extensions
}  // namespace Envoy
