#include <userver/clients/common/response_future.hpp>

#include <algorithm>

#include <clients/common/request_state.hpp>
#include <userver/server/request/task_inherited_data.hpp>
#include <userver/utils/fast_scope_guard.hpp>
#include <userver/utils/trx_tracker.hpp>

#include <userver/clients/smtp/response.hpp>
#include <userver/clients/http/response.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::common {

namespace {

/// Max number of retries during calculating timeout
constexpr int kMaxRetryInTimeout = 5;
/// Base time for exponential backoff algorithm
constexpr long kEBBaseTime = 25;

long MaxRetryTime(short retries) {
    long time_ms = 0;
    for (short int i = 1; i < retries; ++i) {
        time_ms += kEBBaseTime * ((1 << std::min(i - 1, kMaxRetryInTimeout)) + 1);
    }
    return time_ms;
}

long CompleteTimeout(long request_timeout, short retries) {
    return static_cast<long>(static_cast<double>(request_timeout * retries) * 1.1) + MaxRetryTime(retries);
}

engine::Deadline ComputeBaseDeadline(common::RequestState& request_state) {
    return engine::Deadline::FromDuration(
        std::chrono::milliseconds(CompleteTimeout(request_state.timeout(), request_state.retries()))
    );
}

}  // namespace
template <typename T>
ResponseFuture<T>::ResponseFuture(
    engine::Future<std::shared_ptr<T>>&& future,
    std::shared_ptr<common::RequestState> request_state
)
    : future_(std::move(future)),
      deadline_(ComputeBaseDeadline(*request_state)),
      request_state_(std::move(request_state)),
      cancellation_policy_(request_state_->GetCancellationPolicy())
{
    const auto propagated_deadline = request_state_->GetDeadline();
    if (propagated_deadline < deadline_) {
        deadline_ = propagated_deadline;
        was_deadline_propagated_ = true;
    }
}

template <typename T>
ResponseFuture<T>::ResponseFuture(ResponseFuture&& other) noexcept : cancellation_policy_(other.cancellation_policy_) {
    std::swap(future_, other.future_);
    std::swap(deadline_, other.deadline_);
    std::swap(request_state_, other.request_state_);
}

template <typename T>
ResponseFuture<T>& ResponseFuture<T>::operator=(ResponseFuture<T>&& other) noexcept {
    if (&other == this) {
        return *this;
    }
    CancelOrDetach();
    future_ = std::move(other.future_);
    deadline_ = other.deadline_;
    request_state_ = std::move(other.request_state_);
    was_deadline_propagated_ = other.was_deadline_propagated_;
    cancellation_policy_ = other.cancellation_policy_;
    return *this;
}

template <typename T>
ResponseFuture<T>::~ResponseFuture() { CancelOrDetach(); }

template <typename T>
void ResponseFuture<T>::CancelOrDetach() {
    switch (cancellation_policy_) {
        case common::CancellationPolicy::kIgnore:
            Detach();
            break;
        case common::CancellationPolicy::kCancel:
            Cancel();
            break;
    }
}

template <typename T>
void ResponseFuture<T>::Cancel() {
    if (request_state_) {
        request_state_->Cancel();
    }
    Detach();
}

template <typename T>
void ResponseFuture<T>::Detach() {
    future_ = {};
    request_state_.reset();
}

template <typename T>
std::future_status ResponseFuture<T>::Wait(utils::impl::SourceLocation location) {
    utils::trx_tracker::CheckNoTransactions(location);

    switch (future_.wait_until(deadline_)) {
        case engine::FutureStatus::kCancelled: {
            const auto stats = request_state_->easy().get_local_stats();

            // request_ has armed timers to retry the request. Stopping those ASAP.
            CancelOrDetach();

            throw common::CancelException("HTTP response wait was aborted due to task cancellation", stats, common::ErrorKind::kCancel);
        }
        case engine::FutureStatus::kTimeout:
            if (was_deadline_propagated_) {
                server::request::MarkTaskInheritedDeadlineExpired();

                // We allow the physical HTTP request to complete in the background to
                // avoid closing the connection.
                const utils::FastScopeGuard detach_guard([this]() noexcept { Detach(); });

                request_state_->ThrowDeadlineExpiredException();
            }
            return std::future_status::timeout;
        case engine::FutureStatus::kReady:
            return std::future_status::ready;
    }

    UINVARIANT(false, "Invalid engine::FutureStatus");
}

template <typename T>
std::shared_ptr<T> ResponseFuture<T>::Get(utils::impl::SourceLocation location) {
    const auto future_status = Wait(location);
    if (future_status == std::future_status::ready) {
        if (request_state_->IsDeadlineExpired()) {
            server::request::MarkTaskInheritedDeadlineExpired();
        }
        auto response = future_.get();
        Detach();
        return response;
    }

    throw common::TimeoutException("Future timeout", {});  // no local stats available
}

template <typename T>
engine::impl::ContextAccessor* ResponseFuture<T>::TryGetContextAccessor() noexcept {
    return future_.TryGetContextAccessor();
}

}  // namespace clients::http

template class clients::common::ResponseFuture<clients::http::Response>;
template class clients::common::ResponseFuture<clients::smtp::Response>;

USERVER_NAMESPACE_END
