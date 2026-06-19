#pragma once

#include <clients/common/request_state.hpp>
#include <userver/clients/smtp/response.hpp>


USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

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
    engine::Future<std::shared_ptr<smtp::Response>> async_perform_smtp(
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

    std::shared_ptr<smtp::Response> response() const { return response_; }
    std::shared_ptr<smtp::Response> response_move() { return std::move(response_); }

    /// response
    std::shared_ptr<smtp::Response> response_;

    struct EmailFullBufferedData {
        engine::Promise<std::shared_ptr<smtp::Response>> promise;
    };

    std::variant<EmailFullBufferedData> data_;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
