#include <userver/clients/smtp/client.hpp>

#include <chrono>
#include <sstream>
#include <utility>

#include <userver/clients/smtp/error.hpp>
#include <userver/engine/async.hpp>
#include <userver/engine/future.hpp>
#include <userver/utils/rand.hpp>

#include <curl-ev/easy.hpp>
#include <curl-ev/multi.hpp>
#include <curl-ev/ratelimit.hpp>
#include <curl-ev/string_list.hpp>
#include <engine/ev/thread_pool.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

namespace {

constexpr std::string_view kIoThreadName = "smtp";

}  // namespace

Client::Client(ClientSettings settings, engine::TaskProcessor& fs_task_processor)
    : settings_(std::move(settings)),
      fs_task_processor_(fs_task_processor),
      connect_rate_limiter_(std::make_shared<curl::ConnectRateLimiter>()) {
    const auto io_threads = settings_.io_threads ? settings_.io_threads : 1;

    engine::ev::ThreadPoolConfig ev_config;
    ev_config.threads = io_threads;
    ev_config.thread_name = std::string{kIoThreadName};
    thread_pool_ = std::make_unique<engine::ev::ThreadPool>(std::move(ev_config));

    // libcurl synchronously reads some of /etc/* files on init, so the blocking
    // parts are shifted to the fs task processor (same as the HTTP client).
    easy_template_ = engine::AsyncNoTracing(fs_task_processor_, &curl::easy::CreateBlocking).Get();

    multis_.reserve(io_threads);
    engine::AsyncNoTracing(fs_task_processor_, [this, io_threads] {
        for (std::size_t i = 0; i < io_threads; ++i) {
            multis_.push_back(std::make_unique<curl::multi>(thread_pool_->NextThread(), connect_rate_limiter_));
        }
    }).Get();
}

Client::~Client() {
    // All Send() calls are synchronous, so by the time the client is destroyed
    // there are no in-flight easy handles referencing the multis or the thread
    // pool.
    multis_.clear();
    thread_pool_.reset();
}

std::string Client::MakeUrl() const {
    const std::string_view scheme = settings_.encryption == EncryptionType::kSmtps ? "smtps" : "smtp";
    return std::string{scheme} + "://" + settings_.host + ':' + std::to_string(settings_.port);
}

std::shared_ptr<curl::easy> Client::GetBoundEasy() {
    const auto i = utils::RandRange(multis_.size());
    auto& multi = *multis_[i];

    // curl_easy_duphandle() is blocking, so it runs on the fs task processor.
    return engine::AsyncNoTracing(fs_task_processor_, [this, &multi] {
               return easy_template_->GetBoundBlocking(multi);
           }).Get();
}

void Client::Send(const Message& message, engine::Deadline deadline) {
    auto easy = GetBoundEasy();

    easy->set_url(MakeUrl());

    if (settings_.auth) {
        easy->set_user(settings_.auth->login);
        easy->set_password(settings_.auth->password);
    }

    if (settings_.encryption == EncryptionType::kStartTls) {
        easy->set_use_ssl(curl::easy::use_ssl_all);
    }
    if (settings_.encryption != EncryptionType::kNone) {
        easy->set_ssl_verify_peer(settings_.verify_tls);
        easy->set_ssl_verify_host(settings_.verify_tls);
    }

    easy->set_mail_from(message.GetSenderAddress());

    auto recipients = std::make_shared<curl::string_list>();
    for (auto& address : message.GetRecipientAddresses()) {
        recipients->add(std::move(address));
    }
    easy->set_mail_rcpt(std::move(recipients));

    easy->set_upload(true);
    easy->set_source(std::make_shared<std::istringstream>(message.ToString()));

    if (deadline.IsReachable()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline.TimeLeft());
        easy->set_timeout_ms(left.count() > 0 ? left.count() : 1);
    }

    // The handler type is a std::function, so it must be copyable; the move-only
    // promise is therefore held through a shared_ptr.
    auto promise = std::make_shared<engine::Promise<void>>();
    auto future = promise->get_future();

    // The completion handler is invoked in the libev thread; engine::Promise is
    // safe to fulfil from a non-coroutine context. `easy` is kept alive by this
    // stack frame until future.get() returns.
    easy->async_perform([promise](std::error_code ec) {
        if (ec) {
            promise->set_exception(std::make_exception_ptr(SendException("SMTP send failed: " + ec.message(), ec)));
        } else {
            promise->set_value();
        }
    });

    future.get();
}

}  // namespace clients::smtp

USERVER_NAMESPACE_END
