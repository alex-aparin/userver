#include <userver/clients/smtp/request.hpp>

#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#include <userver/clients/http/response_future.hpp>
#include <userver/concurrent/queue.hpp>
#include <userver/engine/future.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/url.hpp>
#include <userver/tracing/span.hpp>
#include <userver/tracing/tags.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/str_icase.hpp>
#include <userver/utils/trivial_map.hpp>

#include <clients/common/easy_wrapper.hpp>
#include <clients/smtp/request_state.hpp>
#include <crypto/helpers.hpp>
#include <engine/ev/watcher/timer_watcher.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

// Request implementation

Request::Request(
    common::impl::EasyWrapper&& wrapper,
    common::RequestStats&& req_stats,
    const std::shared_ptr<common::DestinationStatistics>& dest_stats,
    clients::dns::Resolver* resolver,
    const tracing::TracingManagerBase& tracing_manager
)
    : pimpl_(std::make_shared<
             RequestState>(std::move(wrapper), std::move(req_stats), dest_stats, resolver, tracing_manager))
{
    LOG_TRACE() << "Request::Request()";
    

    if (engine::current_task::ShouldCancel()) {
        throw common::CancelException("Failed to make HTTP request due to task cancellation", {}, common::ErrorKind::kCancel);
    }
}

Request& Request::url(std::string url) & {
    pimpl_->easy().set_url(std::move(url));
    return *this;
}

    /// @overload
Request Request::url(std::string url) && {
 return std::move(this->url(url));   
}

/// Specifies method
Request& Request::from(std::string from) & {
    pimpl_->set_mail_from(std::move(from));
    return *this;
}
/// @overload
Request Request::from(std::string sender) && {
 return std::move(this->from(sender));
}

Request& Request::recipients(std::vector<std::string> recipients) & {
    pimpl_->easy().set_recipients(std::move(recipients));
    return *this;
}
/// @overload
Request Request::recipients(std::vector<std::string> recipients) && {
    return std::move(this->recipients(std::move(recipients)));
}


Request& Request::timeout(long timeout_ms) & {
    pimpl_->set_timeout(timeout_ms);
    return *this;
}

/// Specifies method
Request& Request::subject(std::string subject) & {
return *this;
}
/// @overload
Request Request::subject(std::string subject) && {
    return std::move(this->subject(std::move(subject)));
}

Request& Request::authenticate(std::string user, std::string password) & {
    pimpl_->easy().set_user(user.c_str());
    pimpl_->easy().set_password(password.c_str());
    return *this;
}

Request Request::authenticate(std::string user, std::string password) && {
    return std::move(this->authenticate(std::move(user), std::move(password)));
}

Request& Request::content_type(std::string) & {
    return *this;
}
Request Request::content_type(std::string content_type) && {
    return std::move(this->content_type(std::move(content_type)));
}

Request& Request::message(std::string message) & {
    pimpl_->set_message(std::move(message));
    return *this;
}
Request Request::message(std::string message) && {
    return std::move(this->message(std::move(message)));
}

smtp::ResponseFuture Request::async_perform(utils::impl::SourceLocation location) {
    smtp::ResponseFuture future{pimpl_->async_perform_smtp(location), pimpl_};
    return future;
}


std::shared_ptr<smtp::Response> Request::perform(utils::impl::SourceLocation location) {
    return async_perform(location).Get(location);
}

}  // namespace clients::http

USERVER_NAMESPACE_END
