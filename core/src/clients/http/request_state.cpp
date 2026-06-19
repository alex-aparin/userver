#include <clients/http/request_state.hpp>
#include <userver/utils/overloaded.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/from_string.hpp>
#include <userver/clients/http/websocket_response.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::http {

namespace {
/// Least http code the the downstream service can use to report propagated
/// deadline expiration
constexpr http::Status kLeastHttpCodeForDeadlineExpired{400};

bool IsSetCookie(std::string_view key) {
    const utils::StrIcaseEqual equal;
    return equal(key, USERVER_NAMESPACE::http::headers::kSetCookie);
}

// Not a strict check, but OK for non-header line check
bool IsHttpStatusLineStart(const char* ptr, size_t size) { return (size > 5 && memcmp(ptr, "HTTP/", 5) == 0); }

bool IsHttp11WithCompleteBody(const std::shared_ptr<http::Response> response) {
    constexpr auto kConnectionTokenClose = "close";

    if (response->status_code() == http::Status::kInvalid) {
        return false;
    }

    const auto& headers = response->headers();

    const auto connection_token = utils::FindOrDefault(headers, USERVER_NAMESPACE::http::headers::kConnection, "");
    if (connection_token != kConnectionTokenClose) {
        return false;
    }

    const auto* content_length = utils::FindOrNullptr(headers, USERVER_NAMESPACE::http::headers::kContentLength);
    // complete body check needs only in case of receiving `Content-Length` in a pair of `Connection: close`
    return !content_length || utils::FromString<size_t>(*content_length) == response->body_view().size();
}

char* RfindNotSpace(char* ptr, size_t size) {
    for (char* p = ptr + size - 1; p >= ptr; --p) {
        const char c = *p;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        return p + 1;
    }
    return ptr;
}

}


RequestState::RequestState(
    common::impl::EasyWrapper&& wrapper,
    common::RequestStats&& req_stats,
    const std::shared_ptr<common::DestinationStatistics>& dest_stats,
    clients::dns::Resolver* resolver,
    const tracing::TracingManagerBase& tracing_manager
)
    : common::RequestState(std::move(wrapper), std::move(req_stats), dest_stats, resolver, tracing_manager)
{
    // define header function
    easy().set_header_function(&RequestState::OnHeader);
    easy().set_header_data(this);

    RequestCompleted();
}

RequestState::~RequestState() {
}

engine::Future<std::shared_ptr<http::Response>> RequestState::async_perform(utils::impl::SourceLocation location) {
    WaitForRequestCompletion();
    auto& data = data_.emplace<FullBufferedData>();

    StartNewSpan(location);
    ResetDataForNewRequest();

    auto& span = span_storage_->Get();
    span.AddTag("stream_api", 0);

    // set place for response body
    easy().set_sink(&response_->sink_string());

    auto future = data.promise.get_future();

    if (UpdateTimeoutFromDeadlineAndCheck()) {
        PerformRequest([holder = shared_from_this()](std::error_code err) mutable {
            RequestState::OnRetry(std::move(holder), err);
        });
    }

    return future;
}

engine::Future<void> RequestState::async_perform_stream(
    const std::shared_ptr<Queue>& queue,
        utils::impl::SourceLocation location
) {
    WaitForRequestCompletion();
    auto& data = data_.emplace<StreamData>(queue->GetProducer());

    StartNewSpan(location);
    ResetDataForNewRequest();

    auto& span = span_storage_->Get();
    span.AddTag("stream_api", 1);

    easy().set_write_function(&RequestState::StreamWriteFunction);
    easy().set_write_data(this);
    // Force no retries
    retry_.retries = 1;

    auto future = data.headers_promise.get_future();

    if (UpdateTimeoutFromDeadlineAndCheck()) {
        PerformRequest([holder = shared_from_this()](std::error_code err) mutable {
            RequestState::OnCompleted(std::move(holder), err);
        });
    }

    return future;
}

engine::Future<http::WebSocketResponse> RequestState::async_perform_websocket_handshake(utils::impl::SourceLocation location
) {
    WaitForRequestCompletion();
    auto& data = data_.emplace<WebSocketHandshakeData>();

    StartNewSpan(location);
    ResetDataForNewRequest();

    auto& span = span_storage_->Get();
    span.AddTag("stream_api", 0);

    // set place for response body
    easy().set_sink(&response_->sink_string());
    easy().set_connect_only(2L /** websocket handshake */);
    easy().enable_socket_extraction();

    auto future = data.promise.get_future();

    if (UpdateTimeoutFromDeadlineAndCheck()) {
        PerformRequest([holder = shared_from_this()](std::error_code err) mutable {
            RequestState::OnRetry(std::move(holder), err);
        });
    }

    return future;
}

void RequestState::SetError(std::error_code& err) {
    const utils::Overloaded visitor{
        [this, &err](FullBufferedData& buffered_data) {
            {
                [[maybe_unused]] const auto cleanup = this->response_move();
            }
            auto promise = std::move(buffered_data.promise);
            // The task will wake up and may reuse RequestState.
            promise.set_exception(this->PrepareException(err));
        },
        [](StreamData& stream_data) {
            auto producer = std::move(stream_data.queue_producer);
            // The task will wake up and may reuse RequestState.
            std::move(producer).Reset();
        },
        [this, &err](WebSocketHandshakeData& data) {
            {
                [[maybe_unused]] const auto cleanup = this->response_move();
            }
            auto promise = std::move(data.promise);
            // The task will wake up and may reuse RequestState.
            promise.set_exception(this->PrepareException(err));
        },
    };
    std::visit(visitor, data_);
}

void RequestState::SetResponse(long raw_status_code, tracing::Span& span) {
    response()->SetStatusCode(static_cast<http::Status>(raw_status_code));
    response()->SetStats(easy().get_local_stats());

    if (response()->IsError()) {
        span.AddTag(tracing::kErrorFlag, true);
    }

    middlewares_pipeline_.HookOnCompleted(*this, *response());

    const utils::Overloaded visitor{
            [this](FullBufferedData& buffered_data) {
                auto promise = std::move(buffered_data.promise);
                // The task will wake up and may reuse RequestState.
                promise.set_value(this->response_move());
            },
            [](StreamData& stream_data) {
                auto producer = std::move(stream_data.queue_producer);
                // The task will wake up and may reuse RequestState.
                std::move(producer).Reset();
            },
            [this](WebSocketHandshakeData& data) {
                auto promise = std::move(data.promise);
                // The task will wake up and may reuse RequestState.
                promise.set_value(http::WebSocketResponse(this->response_move(), std::move(this->easy().extracted_socket())));
            }
        };
        std::visit(visitor, data_);
}

void RequestState::SetException(std::exception_ptr& exc) {
        const utils::Overloaded visitor{
        [&exc](FullBufferedData& buffered_data) {
            auto promise = std::move(buffered_data.promise);
            // The task will wake up and may reuse RequestState.
            promise.set_exception(std::move(exc));
        },
        [&exc](StreamData& stream_data) {
            if (!stream_data.headers_promise_set.exchange(true)) {
                auto promise = std::move(stream_data.headers_promise);
                // The task will wake up and may reuse RequestState.
                promise.set_exception(std::move(exc));
            }
        },
        [&exc](WebSocketHandshakeData& data) {
            auto promise = std::move(data.promise);
            promise.set_exception(std::move(exc));
        }
    };
    std::visit(visitor, data_);
}


void RequestState::SetCurrentException() {
    auto exception_handler = utils::Overloaded{
        [](FullBufferedData& data) { data.promise.set_exception(std::current_exception()); },
        [](WebSocketHandshakeData& data) { data.promise.set_exception(std::current_exception()); },
        [](StreamData&) {},
    };
    std::visit(exception_handler, data_);
}

long RequestState::PreCheckErr(std::error_code& err, tracing::Span& span) {
    auto& easy = this->easy();
    if (response()->status_code() == http::Status::kInvalid && !err) {
        // We haven't received the full set of headers, the response is truncated
        err = std::error_code(curl::errc::EasyErrorCode::kRecvError);

        response()->SetStatusCode(http::Status::kInternalServerError);
    }

    // TODO don't swallow errors, report them to StreamedResponse
    auto* stream_data = std::get_if<StreamData>(&data_);
    if (stream_data && !stream_data->headers_promise_set.exchange(true)) {
        LOG_DEBUG() << "Stream API, status code is set (with body)";
        if (!err) {
            stream_data->headers_promise.set_value();
        } else {
            // The task will wake up and may reuse RequestState.
            auto promise = std::move(stream_data->headers_promise);
            promise.set_exception(PrepareException(err));
        }
    }

    const auto raw_status_code = easy.get_response_code();
    const auto status_code = static_cast<http::Status>(raw_status_code);

    CheckResponseDeadline(err, status_code);

    if (err && std::get_if<WebSocketHandshakeData>(&data_) &&
        err == std::error_code(curl::errc::EasyErrorCode::kHttpReturnedError) && status_code != http::Status::kInvalid)
    {
        // curl expects 101 for WebSocket, treats other statuses as error.
        // Ignore error if got complete HTTP response (e.g. 401, 403).
        err = {};
    }

    if (testsuite_config_ && !err) {
        const auto& headers = response()->headers();
        err = TestsuiteResponseHook(status_code, headers, span);
    }

    if (is_incomplete_tls_connection_close_expected_.load(std::memory_order_acquire) && !stream_data &&
        IsHttp11WithCompleteBody(response()))
    {
        // The response says "HTTP/1.1", the full body is read.
        // It's a transport error, but not a HTTP request/respose error.
        err = {};
    }
    return raw_status_code;
}

long RequestState::GetResponseRawStatusCode() {
    return response()->status_code();
}
void RequestState::ResetResponse() {
response_ = std::make_shared<http::Response>();
    response_->SetStatusCode(http::Status::kInvalid);
}
void RequestState::ClearResponseBody() {
    UASSERT(response_);
    response_->sink_string().clear();
    response_->body().clear();
}

void RequestState::ParseSingleCookie(const char* ptr, size_t size) {
    if (auto cookie = server::http::Cookie::FromString(std::string_view(ptr, size))) {
        [[maybe_unused]] auto [it, ok] = response_->cookies().emplace(cookie->Name(), std::move(*cookie));
        if (!ok) {
            LOG_WARNING() << "Failed to add cookie '" + it->first + "', already added";
        }
    }
}

void RequestState::ParseHeader(char* ptr, size_t size) try
{
    /* It is a fast path in curl's thread (io thread).  Creation of tmp
     * std::string, boost::trim_right_if(), etc. is too expensive. */

    auto* end = RfindNotSpace(ptr, size);
    if (ptr == end) {
        const auto status_code = static_cast<http::Status>(easy().get_response_code());
        response()->SetStatusCode(status_code);
        return;
    }
    *end = '\0';

    const char* col_pos = static_cast<const char*>(memchr(ptr, ':', size));
    if (col_pos == nullptr) {
        if (IsHttpStatusLineStart(ptr, size)) {
            if (!response()->headers().empty()) {
                LOG_INFO() << "Drop headers: " << (response_->headers() | boost::adaptors::map_keys);
            }
            // In case of redirect drop 1st response headers
            response_->headers().clear();
        }
        return;
    }

    std::string key(ptr, col_pos - ptr);

    ++col_pos;

    if (IsSetCookie(key)) {
        ParseSingleCookie(col_pos, end - col_pos);
        return;
    }

    // From https://tools.ietf.org/html/rfc7230#page-22 :
    //
    // header-field   = field-name ":" OWS field-value OWS
    // OWS            = *( SP / HTAB )
    while (end != col_pos && (*col_pos == ' ' || *col_pos == '\t')) {
        ++col_pos;
    }

    std::string value(col_pos, end - col_pos);
    response_->headers().emplace(std::move(key), std::move(value));
} catch (const std::exception& e) {
    LOG_ERROR() << "Failed to parse header: " << e.what();
}

size_t RequestState::OnHeader(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* self = static_cast<RequestState*>(userdata);
    const std::size_t data_size = size * nmemb;
    if (self) {
        self->ParseHeader(static_cast<char*>(ptr), data_size);
    }
    return data_size;
}

size_t RequestState::StreamWriteFunction(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t actual_size = size * nmemb;
    RequestState& rs = *static_cast<RequestState*>(userdata);
    auto* stream_data = std::get_if<StreamData>(&rs.data_);
    UASSERT(stream_data);

    LOG_DEBUG()
        << fmt::format("Got bytes in stream API chunk, chunk of ({} bytes)", actual_size)
        << tracing::impl::LogSpanAsLastNoCurrent{rs.span_storage_->Get()};

    std::string buffer(ptr, actual_size);
    auto& queue_producer = stream_data->queue_producer;

    if (!stream_data->headers_promise_set.exchange(true)) {
        stream_data->headers_promise.set_value();
        LOG_DEBUG() << "Stream API, status code is set (with body)";
    }

    if (queue_producer.PushNoblock(std::move(buffer))) {
        return actual_size;
    }
    LOG_DEBUG() << "PushNoblock() has failed";

    if (queue_producer.Queue()->NoMoreConsumers()) {
        return actual_size;
    }

    LOG_DEBUG() << "There are some alive consumers";

    UINVARIANT(false, "not implemented CURL_WRITEFUNC_PAUSE TAXICOMMON-5611");

    return CURL_WRITEFUNC_PAUSE;
}

bool RequestState::IsDeadlineExpiredResponse(long raw_status_code) {
    // There are two cases where deadline expires in the downstream service:
    //
    // 1. We used all of our own deadline for the attempt. Our deadline and
    // the downstream service deadline have expired at about the same time.
    // This case SHOULD NOT be retried.
    //
    // 2. We set a "small" timeout for the attempt that is less than our
    // own deadline. The downstream service deadline (taken from the
    // timeout) has expired, but deadline of the current task has not yet
    // expired. This case SHOULD be retried.
    return (
        deadline_propagation_config_.update_header && 
        static_cast<http::Status>(raw_status_code) >= kLeastHttpCodeForDeadlineExpired &&
        response_->headers().contains(USERVER_NAMESPACE::http::headers::kXYaTaxiDeadlineExpired)
    );
}

}  // namespace clients::http

USERVER_NAMESPACE_END
