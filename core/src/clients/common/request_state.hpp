#pragma once

#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <clients/http/middlewares/pipeline.hpp>
#include <userver/clients/dns/resolver_fwd.hpp>
#include <userver/clients/http/config.hpp>
#include <userver/clients/http/error.hpp>
#include <userver/clients/http/form.hpp>
#include <userver/clients/http/request.hpp>
#include <userver/clients/http/response_future.hpp>
#include <userver/concurrent/queue.hpp>
#include <userver/crypto/certificate.hpp>
#include <userver/crypto/private_key.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/future.hpp>
#include <userver/engine/single_consumer_event.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/url.hpp>
#include <userver/tracing/in_place_span.hpp>
#include <userver/tracing/manager.hpp>
#include <userver/tracing/span.hpp>
#include <userver/tracing/tags.hpp>
#include <userver/utils/impl/wait_token_storage.hpp>
#include <userver/utils/not_null.hpp>
#include <userver/utils/zstring_view.hpp>

#include <clients/common/destination_statistics.hpp>
#include <clients/common/easy_wrapper.hpp>
#include <clients/http/testsuite.hpp>
#include <crypto/helpers.hpp>
#include <engine/ev/watcher/timer_watcher.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::http {
    class ConnectTo;
}

namespace clients::common {

constexpr std::string_view kHeaderExpect = "Expect";

class RequestState : public std::enable_shared_from_this<RequestState> {
public:
    RequestState(
        impl::EasyWrapper&&,
        common::RequestStats&& req_stats,
        const std::shared_ptr<common::DestinationStatistics>& dest_stats,
        clients::dns::Resolver* resolver,
        const tracing::TracingManagerBase& tracing_manager
    );
    virtual ~RequestState();

    using Queue = concurrent::StringStreamQueue;



    /// set redirect flags
    void follow_redirects(bool follow);
    /// set verify flags
    void verify(bool verify);
    /// set file holding one or more certificates to verify the peer with
    void ca_info(utils::zstring_view file_path);
    /// set certificate to verify the peer with
    void ca(crypto::Certificate cert);
    /// set CRL-file
    void crl_file(utils::zstring_view file_path);
    /// set private key and certificate from memory
    void client_key_cert(crypto::PrivateKey pkey, crypto::Certificate cert);
    /// Set HTTP version
    void http_version(curl::easy::http_version_t version);
    /// set timeout value
    void set_timeout(long timeout_ms);
    /// set number of retries
    void retry(short retries, bool on_fails);
    /// set unix socket as transport instead of TCP
    void unix_socket_path(utils::zstring_view path);
    /// set connect_to option
    void connect_to(const http::ConnectTo& connect_to);
    /// sets proxy to use
    void proxy(utils::zstring_view value);
    /// sets proxy auth type to use
    void proxy_auth_type(curl::easy::proxyauth_t value);
    /// sets proxy auth type and credentials to use
    void http_auth_type(
        curl::easy::httpauth_t value,
        bool auth_only,
        utils::zstring_view user,
        utils::zstring_view password
    );

    /// get timeout value in milliseconds
    long timeout() const { return original_timeout_.count(); }
    /// get retries count
    short retries() const { return retry_.retries; }

    engine::Deadline GetDeadline() const noexcept;
    /// true iff *we detected* that the deadline has expired
    bool IsDeadlineExpired() const noexcept;

    [[noreturn]] void ThrowDeadlineExpiredException();

    /// cancel request
    void Cancel();

    void SetDestinationMetricNameAuto(std::string destination);

    void SetDestinationMetricName(const std::string& destination);

    void SetTestsuiteConfig(const std::shared_ptr<const http::TestsuiteConfig>& config);

    void SetAllowedUrlsExtra(const std::vector<std::string>& urls);

    void DisableReplyDecoding();

    void SetCancellationPolicy(common::CancellationPolicy cp);

    common::CancellationPolicy GetCancellationPolicy() const;

    void SetDeadlinePropagationConfig(const http::DeadlinePropagationConfig& deadline_propagation_config);

    curl::easy& easy() { return easy_.Easy(); }
    const curl::easy& easy() const { return easy_.Easy(); }

    void SetMiddlewaresList(const std::vector<utils::NotNull<http::MiddlewareBase*>>& middlewares);
    void SetIncompleteTlsConnectionCloseExpected(bool expect);
    void SetLoggedUrl(std::string url);
    void SetUrlTemplate(std::string url_template);
    void SetMethod(clients::http::HttpMethod method);
    void data(std::string data);
    void SetEasyTimeout(std::chrono::milliseconds timeout);

    void SetTracingManager(const tracing::TracingManagerBase&);

    void SetWaitToken(utils::impl::WaitTokenStorageLock&&);

    /// true if proxy was set using proxy method
    bool IsProxySet() const;

    http::MiddlewareRequest GetEditableRequestInstance();

    void set_mail_from(std::string&& mail_from);

    void set_message(std::string&& message);

    virtual bool IsResponseEmpty() = 0;

protected:
    static std::error_code TestsuiteResponseHook(http::Status status_code, const http::Headers& headers, tracing::Span& span);
    /// final callback that calls user callback and set value in promise
    static void OnCompleted(std::shared_ptr<RequestState>, std::error_code err);
    /// retry callback
    static void OnRetry(std::shared_ptr<RequestState>, std::error_code err);

    static size_t MessageBodyReadFunction(void* ptr, size_t size, size_t nmemb, void* userdata);

    /// certificate function curl callback
    static curl::native::CURLcode OnCertificateRequest(void* curl, void* sslctx, void* userdata) noexcept;

    /// simply run perform_request if there is now errors from timer
    void OnRetryTimer(std::error_code err);
    /// run curl async_request, called once per attempt
    void PerformRequest(curl::easy::handler_type handler);

    void UpdateTimeoutFromDeadline(std::chrono::milliseconds backoff);
    [[nodiscard]] bool UpdateTimeoutFromDeadlineAndCheck(std::chrono::milliseconds backoff = {});
    void UpdateTimeoutHeader();
    void HandleDeadlineAlreadyPassed();
    void CheckResponseDeadline(std::error_code& err, http::Status status_code);
    virtual bool IsDeadlineExpiredResponse(long raw_status_code) = 0;
    bool ShouldRetryResponse();


    virtual void SetError(std::error_code& err) = 0;
    virtual void SetResponse(long raw_status_code, tracing::Span& span) = 0;
    virtual void SetException(std::exception_ptr& exc) = 0;
    virtual void SetCurrentException() = 0;
    virtual long PreCheckErr(std::error_code& err, tracing::Span& span) = 0;
    virtual long GetResponseRawStatusCode() = 0;
    virtual void ResetResponse() = 0;
    virtual void ClearResponseBody() = 0;
    

    const std::string& GetLoggedOriginalUrl() const noexcept;
    std::string_view GetLoggedEffectiveUrl() noexcept;

    

    void AccountResponse(std::error_code err);
    std::exception_ptr PrepareException(std::error_code err);

    void ResetDataForNewRequest();
    void ApplyTestsuiteConfig();
    void StartNewSpan(utils::impl::SourceLocation location);
    void StartStats();

    template <typename Func>
    void WithRequestStats(const Func& func);

    void ResolveTargetAddress(clients::dns::Resolver& resolver);

    void ResetRequestCompletion();
    void RequestCompleted();
    void WaitForRequestCompletion();

    // should be the first member to prevent HttpClient destruction before destruction of RequestState fields
    utils::impl::WaitTokenStorageLock wait_token_;
    /// curl handler wrapper
    impl::EasyWrapper easy_;
    common::RequestStats stats_;
    std::shared_ptr<common::RequestStats> dest_req_stats_;
    common::CancellationPolicy cancellation_policy_{common::CancellationPolicy::kCancel};

    std::shared_ptr<common::DestinationStatistics> dest_stats_;
    std::string destination_metric_name_;

    std::shared_ptr<const http::TestsuiteConfig> testsuite_config_;
    std::vector<std::string> allowed_urls_extra_;

        std::string message_;
    size_t bytes_to_read_;

    crypto::PrivateKey pkey_;
    crypto::Certificate cert_;
    crypto::Certificate ca_;

    /// the timeout value provided by user (or the default)
    std::chrono::milliseconds original_timeout_;
    /// the timeout propagated to the downstream service
    std::chrono::milliseconds remote_timeout_;

    http::DeadlinePropagationConfig deadline_propagation_config_;
    /// deadline from current task
    engine::Deadline deadline_;
    bool timeout_updated_by_deadline_{false};
    bool deadline_expired_{false};

    utils::NotNull<const tracing::TracingManagerBase*> tracing_manager_;
    /// struct for reties
    struct {
        /// maximum number of retries
        short retries{1};
        /// current retry
        short current{1};
        /// flag for treating network errors as reason for retry
        bool on_fails{false};
        /// pointer to timer
        std::optional<engine::ev::TimerWatcher> timer;
    } retry_;

    std::optional<tracing::InPlaceSpan> span_storage_;
    std::optional<std::string> log_url_;

    std::optional<std::string> url_template_;
    http::HttpMethod method_{http::HttpMethod::kGet};

    std::atomic<bool> is_cancelled_{false};
    std::atomic<bool> is_incomplete_tls_connection_close_expected_{false};
    std::array<char, CURL_ERROR_SIZE> errorbuffer_{};

    clients::dns::Resolver* resolver_{nullptr};
    std::optional<std::string> proxy_url_;
    http::MiddlewaresPipeline middlewares_pipeline_;

    engine::SingleConsumerEvent request_completed_{engine::SingleConsumerEvent::NoAutoReset{}};
};

}  // namespace clients::http

USERVER_NAMESPACE_END
