#pragma once

/// @file userver/clients/smtp/client.hpp
/// @brief @copybrief clients::smtp::Client

#include <userver/moodycamel/concurrentqueue_fwd.h>
#include <userver/clients/smtp/config.hpp>
#include <userver/clients/smtp/request.hpp>
#include <userver/clients/common/client_core.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/utils/fast_pimpl.hpp>
#include <userver/utils/impl/internal_tag_fwd.hpp>
#include <userver/utils/not_null.hpp>
#include <userver/utils/periodic_task.hpp>
#include <userver/utils/statistics/fwd.hpp>
#include <userver/utils/swappingsmart.hpp>

USERVER_NAMESPACE_BEGIN

namespace curl {
class easy;
class multi;
class ConnectRateLimiter;
}  // namespace curl

namespace engine::ev {
class ThreadPool;
}  // namespace engine::ev

namespace clients::common {
struct InstanceStatistics;
class Statistics;
namespace impl {
class EasyWrapper;
}  // namespace impl
}

namespace clients::smtp {

/// @ingroup userver_clients
///
/// @brief SMTP client that returns a SMTP request builder from
/// CreateRequest().
///
/// Usually retrieved from @ref components::SmtpClient component.
///
/// ## Example usage:
///
class Client : public common::ClientBase{
public:
    Client(utils::impl::InternalTag, ClientSettings settings, engine::TaskProcessor& fs_task_processor);

    Client(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(const Client&) = delete;
    Client& operator=(Client&&) = delete;

    ~Client();

        // For internal use only.
    //void SetConfig(const impl::Config&);

        /// @brief Returns a HTTP request builder type with preset values of
    /// User-Agent and some of the Testsuite stuff (if any).
    ///
    /// @note This method does not apply middlewares, they could be applied
    /// manually using @ref clients::http::Request::SetMiddlewaresList()
    /// @note This method is thread-safe despite being non-const.
    Request CreateRequest();

        // For internal use only.
    void SetConfig(const impl::Config&);
private:
    std::shared_ptr<curl::easy> TryDequeueIdle() noexcept;
    void ReinitEasy();
     size_t FindMultiIndex(const curl::multi*) const;

         // Functions for EasyWrapper that must be noexcept, as they are called from
    // the EasyWrapper destructor.
    friend class common::impl::EasyWrapper;
    virtual void IncPending() noexcept override { ++pending_tasks_; }
    virtual void DecPending() noexcept override { --pending_tasks_; }
    virtual void PushIdleEasy(std::shared_ptr<curl::easy>&& easy) noexcept override ;

    std::atomic<std::size_t> pending_tasks_{0};
    std::shared_ptr<common::DestinationStatistics> destination_statistics_;
    std::unique_ptr<engine::ev::ThreadPool> thread_pool_;
    std::vector<common::Statistics> statistics_;
    std::vector<std::unique_ptr<curl::multi>> multis_;

    common::CancellationPolicy cancellation_policy_;

    static constexpr size_t kIdleQueueSize = 616;
    static constexpr size_t kIdleQueueAlignment = 8;
    using IdleQueueTraits = moodycamel::ConcurrentQueueDefaultTraits;
    using IdleQueueValue = std::shared_ptr<curl::easy>;
    using IdleQueue = moodycamel::ConcurrentQueue<IdleQueueValue, IdleQueueTraits>;
    utils::FastPimpl<IdleQueue, kIdleQueueSize, kIdleQueueAlignment> idle_queue_;

    engine::TaskProcessor& fs_task_processor_;

    utils::SwappingSmart<const curl::easy> easy_;
    utils::PeriodicTask easy_reinit_task_;

    std::shared_ptr<curl::ConnectRateLimiter> connect_rate_limiter_;
    clients::dns::Resolver* resolver_{nullptr};
    utils::NotNull<const tracing::TracingManagerBase*> tracing_manager_;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
