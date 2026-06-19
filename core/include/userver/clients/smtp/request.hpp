#pragma once

/// @file userver/clients/http/request.hpp
/// @brief @copybrief clients::http::Request

#include <memory>
#include <string_view>
#include <vector>

#include <userver/clients/dns/resolver_fwd.hpp>
#include <userver/clients/smtp/response.hpp>
#include <userver/clients/smtp/response_future.hpp>
#include <userver/concurrent/queue.hpp>
#include <userver/crypto/certificate.hpp>
#include <userver/crypto/private_key.hpp>
#include <userver/utils/impl/internal_tag_fwd.hpp>
#include <userver/utils/impl/source_location.hpp>
#include <userver/utils/not_null.hpp>

USERVER_NAMESPACE_BEGIN

namespace tracing {
class TracingManagerBase;
}  // namespace tracing

namespace utils::impl {
class WaitTokenStorageLock;
}  // namespace utils::impl

namespace clients::common {
    namespace impl {
class EasyWrapper;
}  // namespace impl

class RequestStats;
class DestinationStatistics;
}

/// HTTP client helpers
namespace clients::smtp {

class RequestState;



/// @brief Class for creating and performing new http requests, usually retrieved from @ref clients::http::Client.
class Request final {
public:
    /// Request cookies container type

    /// @cond
    // For internal use only.
    explicit Request(common::impl::EasyWrapper&& wrapper, common::RequestStats&& req_stats,
        const std::shared_ptr<common::DestinationStatistics>& dest_stats,
        clients::dns::Resolver* resolver,
        const tracing::TracingManagerBase& tracing_manager);
    /// @endcond

    Request& url(std::string url) &;
    /// @overload
    Request url(std::string url) &&;

    /// Specifies method
    Request& from(std::string sender) &;
    /// @overload
    Request from(std::string sender) &&;

    /// Specifies method
    Request& recipients(std::vector<std::string> recipients) &;
    /// @overload
    Request recipients(std::vector<std::string> recipients) &&;


Request& timeout(long timeout_ms) & ;
Request timeout(long timeout_ms) && { return std::move(this->timeout(timeout_ms)); }

    /// Specifies method
    Request& subject(std::string subject) &;
    /// @overload
    Request subject(std::string subject) &&;

    /// Specifies method
    Request& authenticate(std::string user, std::string password) &;
    /// @overload
    Request authenticate(std::string user, std::string password) &&;

    Request& content_type(std::string) &;
    Request content_type(std::string) &&;

    Request& message(std::string) &;
    Request message(std::string) &&;

    /// Perform request asynchronously.
    ///
    /// Works well with @ref engine::WaitAny(), @ref engine::WaitAnyFor(), and @ref engine::WaitUntil() functions:
    /// @snippet src/clients/http/client_wait_test.cpp HTTP Client - waitany
    ///
    /// Refrain from reusing the Request object.
    /// Though it might be possible to reuse it after extracting data from ResponseFuture, a subsequent async_perform
    /// or perform call could be delayed until the previous request fully completes. This delay can occur if the
    /// previous request either timed out or was canceled.
    /// Future versions might entirely forbid Request objects reuse.
    [[nodiscard]] smtp::ResponseFuture async_perform(
        utils::impl::SourceLocation location = utils::impl::SourceLocation::Current()
    );

    /// Calls async_perform and wait for timeout_ms on a future. Default time  for waiting will be timeout value if it
    /// was set. If error occurred it will be thrown as exception.
    ///
    /// Refrain from reusing the Request object.
    /// Though it might be possible to reuse it after extracting data from ResponseFuture, a subsequent async_perform
    /// or perform call could be delayed until the previous request fully completes. This delay can occur if the
    /// previous request either timed out or was canceled.
    /// Future versions might entirely forbid Request objects reuse.
    [[nodiscard]] std::shared_ptr<smtp::Response> perform(
        utils::impl::SourceLocation location = utils::impl::SourceLocation::Current()
    );

private:
    std::shared_ptr<RequestState> pimpl_;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
