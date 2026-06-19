#pragma once

#include <userver/clients/http/middlewares/base.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::http {

class MiddlewaresPipeline final {
public:
    MiddlewaresPipeline() = default;
    explicit MiddlewaresPipeline(utils::span<const utils::NotNull<MiddlewareBase*>> middlewares);

    void HookPerformRequest(common::RequestState& request);

    void HookCreateSpan(common::RequestState& request, tracing::Span& span);

    void HookOnCompleted(common::RequestState& request, Response& response);

    void HookOnError(common::RequestState& request, std::error_code ec);

    bool HookOnRetry(common::RequestState& request);

private:
    utils::span<const utils::NotNull<MiddlewareBase*>> middlewares_;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
