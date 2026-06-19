#include <userver/clients/smtp/client.hpp>

#include <chrono>
#include <cstdlib>
#include <limits>

#include <moodycamel/concurrentqueue.h>

#include <userver/crypto/openssl.hpp>
#include <userver/logging/log.hpp>
#include <userver/tracing/manager.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/rand.hpp>
#include <userver/utils/string_literal.hpp>
#include <userver/utils/userver_info.hpp>

#include <clients/common/destination_statistics.hpp>
#include <clients/common/easy_wrapper.hpp>
#include <curl-ev/multi.hpp>
#include <curl-ev/ratelimit.hpp>
#include <engine/ev/thread_pool.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {
namespace {

constexpr utils::StringLiteral kIoThreadName = "curl-smtp";
constexpr std::chrono::minutes kEasyReinitPeriod{1};

// cURL accepts options as long, but we use size_t to avoid writing checks.
// Clamp too high values to LONG_MAX, it shouldn't matter for these magnitudes.
long ClampToLong(size_t value) { return std::min<size_t>(value, std::numeric_limits<long>::max()); }

const tracing::TracingManagerBase* GetTracingManager(const ClientSettings& settings) {
    if (settings.tracing_manager) {
        return settings.tracing_manager;
    }

    static const tracing::GenericTracingManager kNoopTracing{tracing::Format{0}, tracing::Format{0}};
    return &kNoopTracing;
}

}  // namespace

Client::Client(utils::impl::InternalTag, ClientSettings settings, engine::TaskProcessor& fs_task_processor)
    : fs_task_processor_(fs_task_processor),
    destination_statistics_(std::make_shared<common::DestinationStatistics>()),
    statistics_(settings.io_threads),
    connect_rate_limiter_(std::make_shared<curl::ConnectRateLimiter>()),
    tracing_manager_(GetTracingManager(settings))
{
    const auto io_threads = settings.io_threads;
    const auto& thread_name_prefix = settings.thread_name_prefix;

    engine::ev::ThreadPoolConfig ev_config;
    ev_config.threads = io_threads;
    ev_config.thread_name.assign(
        thread_name_prefix.empty() ? std::string{kIoThreadName} : utils::StrCat(kIoThreadName, "-", thread_name_prefix)
    );
    thread_pool_ = std::make_unique<engine::ev::ThreadPool>(std::move(ev_config));

    ReinitEasy();

    multis_.reserve(io_threads);

    // libcurl synchronously reads some of /etc/* files.
    // As we want httpclient to be non-blocking, we have to shift curl's init code
    // to a fs task processor.
    engine::AsyncNoSpan(fs_task_processor_, [this, io_threads] {
        for (std::size_t i = 0; i < io_threads; ++i) {
            multis_.push_back(std::make_unique<curl::multi>(thread_pool_->NextThread(), connect_rate_limiter_));
        }
    }).Get();

    easy_reinit_task_.Start("http_easy_reinit", utils::PeriodicTask::Settings(kEasyReinitPeriod), [this] {
        ReinitEasy();
    });

    //SetConfig({});
}

Client::~Client() {
    easy_reinit_task_.Stop();

    // We have to destroy *this only when all the requests are finished, because
    // otherwise `multis_` and `thread_pool_` are destroyed and pending requests
    // cause UB (e.g. segfault).
    //
    // We can not refcount *this in `EasyWrapper` because that leads to
    // destruction of `thread_pool_` in the thread of the thread pool (that's an
    // UB).
    //
    // Best solution so far: track the pending tasks and wait for them to drop to
    // zero.
    while (pending_tasks_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    while (TryDequeueIdle());

    multis_.clear();
    thread_pool_.reset();
}

Request Client::CreateRequest() {
    auto request = [this] {
        auto easy = TryDequeueIdle();
        if (easy) {
            auto idx = FindMultiIndex(easy->GetMulti());
            auto wrapper = common::impl::EasyWrapper{std::move(easy), *this};
            return Request{
                std::move(wrapper),
                statistics_[idx].CreateRequestStats(),
                destination_statistics_,
                resolver_,
                *tracing_manager_.GetBase()
            };
        } else {
            auto i = utils::RandRange(multis_.size());
            auto& multi = multis_[i];

            try {
                auto wrapper =
                    engine::AsyncNoSpan(fs_task_processor_, [this, &multi] {
                        return common::impl::EasyWrapper{easy_.Get()->GetBoundBlocking(*multi), *this};
                    }).Get();
                return Request{
                    std::move(wrapper),
                    statistics_[i].CreateRequestStats(),
                    destination_statistics_,
                    resolver_,
                    *tracing_manager_.GetBase()
                };
            } catch (engine::WaitInterruptedException&) {
                throw clients::common::CancelException("wait interrupted", {}, common::ErrorKind::kCancel);
            } catch (engine::TaskCancelledException&) {
                throw clients::common::CancelException("task cancelled", {}, common::ErrorKind::kCancel);
            }
        }
    }();

    //if (testsuite_config_) {
    //    request.SetTestsuiteConfig(testsuite_config_);
    //}
    //auto urls = allowed_urls_extra_.Read();
    //request.SetAllowedUrlsExtra(*urls);
//
    //if (user_agent_) {
    //    request.user_agent(*user_agent_);
    //}
//
    //request.SetDeadlinePropagationConfig(deadline_propagation_config_);
    //request.SetCancellationPolicy(cancellation_policy_);

    return request;
}

void Client::ReinitEasy() {
    easy_.Set(utils::CriticalAsync(fs_task_processor_, "http_easy_reinit", &curl::easy::CreateBlocking).Get());
}

size_t Client::FindMultiIndex(const curl::multi* multi) const {
    for (size_t i = 0; i < multis_.size(); i++) {
        if (multis_[i].get() == multi) {
            return i;
        }
    }
    UASSERT_MSG(false, "Unknown multi");
    throw std::logic_error("Unknown multi");
}

void Client::PushIdleEasy(std::shared_ptr<curl::easy>&& easy) noexcept {
    try {
        easy->reset();
        idle_queue_->enqueue(std::move(easy));
    } catch (const std::exception& e) {
        LOG_ERROR() << e;
    }

    DecPending();
}

std::shared_ptr<curl::easy> Client::TryDequeueIdle() noexcept {
    std::shared_ptr<curl::easy> result;
    if (!idle_queue_->try_dequeue(result)) {
        return {};
    }
    return result;
}

void Client::SetConfig(const impl::Config& config) {
    const auto pool_size = ClampToLong(config.connection_pool_size / multis_.size());
    if (pool_size * multis_.size() != config.connection_pool_size) {
        LOG_DEBUG()
            << "SetConnectionPoolSize() rounded pool size for each multi (" << config.connection_pool_size << "/"
            << multis_.size() << " rounded to " << pool_size << ")";
    }
    for (auto& multi : multis_) {
        multi->SetConnectionCacheSize(pool_size);
    }
}

}  // namespace clients::http

USERVER_NAMESPACE_END
