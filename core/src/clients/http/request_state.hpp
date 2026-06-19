#pragma once

#include <clients/common/request_state.hpp>
#include <userver/clients/http/response.hpp>


USERVER_NAMESPACE_BEGIN

namespace clients::http {

class RequestState : public common::RequestState {
public:
    RequestState(
        common::impl::EasyWrapper&&,
        common::RequestStats&& req_stats,
        const std::shared_ptr<common::DestinationStatistics>& dest_stats,
        clients::dns::Resolver* resolver,
        const tracing::TracingManagerBase& tracing_manager
    );
    ~RequestState();

    /// Perform async http request
    engine::Future<std::shared_ptr<http::Response>> async_perform(
        utils::impl::SourceLocation location = utils::impl::SourceLocation::Current()
    );

    /// Perform streaming http request, returns headers future
    engine::Future<void> async_perform_stream(
        const std::shared_ptr<Queue>& queue,
        utils::impl::SourceLocation location = utils::impl::SourceLocation::Current()
    );

    engine::Future<http::WebSocketResponse> async_perform_websocket_handshake(
        utils::impl::SourceLocation location = utils::impl::SourceLocation::Current()
    );

private:
    virtual void SetError(std::error_code& err) override;
    virtual void SetResponse(long raw_status_code, tracing::Span& span) override;
    virtual void SetException(std::exception_ptr& exc) override;
    virtual void SetCurrentException() override;
    virtual long PreCheckErr(std::error_code& err, tracing::Span& span) override;
    virtual long GetResponseRawStatusCode() override;
    virtual void ResetResponse() override;
    virtual void ClearResponseBody() override;
    virtual bool IsResponseEmpty() override { return !response_;  }
    virtual bool IsDeadlineExpiredResponse(long raw_status_code) override;

    /// parse one header
    void ParseHeader(char* ptr, size_t size);
    void ParseSingleCookie(const char* ptr, size_t size);

    /// header function curl callback
    static size_t OnHeader(void* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t StreamWriteFunction(char* ptr, size_t size, size_t nmemb, void* userdata);
        std::shared_ptr<http::Response> response() const { return response_; }
    std::shared_ptr<http::Response> response_move() { return std::move(response_); }

    struct StreamData {
        StreamData(Queue::Producer&& queue_producer)
            : queue_producer(std::move(queue_producer))
        {}

        Queue::Producer queue_producer;
        std::atomic<bool> headers_promise_set{false};
        engine::Promise<void> headers_promise;
    };

    struct FullBufferedData {
        engine::Promise<std::shared_ptr<http::Response>> promise;
    };

    struct WebSocketHandshakeData {
        engine::Promise<http::WebSocketResponse> promise;
    };

    
    /// response
    std::shared_ptr<http::Response> response_;

    std::variant<FullBufferedData, StreamData, WebSocketHandshakeData> data_;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
