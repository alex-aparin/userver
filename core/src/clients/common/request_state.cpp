#include <clients/common/request_state.hpp>

#include <algorithm>
#include <chrono>
#include <string_view>

#include <cryptopp/osrng.h>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <curl-ev/error_code.hpp>
#include <userver/baggage/baggage.hpp>
#include <userver/clients/dns/resolver.hpp>
#include <userver/clients/http/connect_to.hpp>
#include <userver/clients/http/websocket_response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/url.hpp>
#include <userver/server/request/task_inherited_data.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/encoding/hex.hpp>
#include <userver/utils/from_string.hpp>
#include <userver/utils/overloaded.hpp>
#include <userver/utils/rand.hpp>
#include <userver/utils/text_light.hpp>
#include <userver/utils/zstring_view.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::common {

namespace {
namespace nolint {
// NOLINT(bugprone-forward-declaration-namespace) due to CryptoPP::Source
using UnusedConfigSourceFwd = dynamic_config::Source&;
}  // namespace nolint

/// Default timeout
constexpr auto kDefaultTimeout = std::chrono::milliseconds{100};
/// Maximum number of redirects
constexpr long kMaxRedirectCount = 10;
/// Max power value for exponential backoff algorithm
constexpr int kEBMaxPower = 5;
/// Base time for exponential backoff algorithm
constexpr auto kEBBaseTime = std::chrono::milliseconds{25};
/// Least http code that we treat as bad for exponential backoff algorithm
constexpr http::Status kLeastBadHttpCodeForEB{500};


constexpr http::Status kFakeHttpErrorCode{599};

constexpr utils::TrivialBiMap kTestsuiteActions = [](auto selector) {
    return selector()
        .Case("timeout", curl::errc::EasyErrorCode::kOperationTimedout)
        .Case("network", curl::errc::EasyErrorCode::kCouldNotConnect);
};

constexpr std::string_view kTestsuiteSupportedErrorsKey = "X-Testsuite-Supported-Errors";

std::string_view GetTestsuiteSupportedErrors() {
    static_assert(kTestsuiteActions.size() == 2, "Fix the below line");
    return "network,timeout";
}

std::string ToString(http::HttpMethod method) { return std::string{ToStringView(method)}; }


// TODO: very low-level, do it in another place
void SetBaggageHeader(curl::easy& e) {
    const auto* baggage = baggage::kInheritedBaggage.GetOptional();
    if (baggage != nullptr) {
        LOG_DEBUG() << fmt::format("Send baggage: {}", baggage->ToString());
        e.add_header(
            USERVER_NAMESPACE::http::headers::kXBaggage,
            baggage->ToString(),
            curl::easy::EmptyHeaderAction::kDoNotSend,
            curl::easy::DuplicateHeaderAction::kReplace
        );
    }
}

std::exception_ptr PrepareDeadlinePassedException(std::string_view url, common::LocalStats stats) {
    return std::make_exception_ptr(common::CancelException(
        fmt::format("Timeout happened (deadline propagation), url: {}", url),
        stats,
        common::ErrorKind::kDeadlinePropagation
    ));
}

bool IsPrefix(const std::string& url, const std::vector<std::string>& prefixes) {
    return !(std::find_if(prefixes.begin(), prefixes.end(), [&url](const std::string& prefix) {
                 return utils::text::StartsWith(url, prefix);
             }) == prefixes.end());
}

class MaybeOwnedUrl final {
public:
    MaybeOwnedUrl(const std::string& proxy_url, curl::easy& easy) {
        if (!proxy_url.empty()) {
            url_if_with_proxy_.emplace();

            std::error_code ec;
            url_if_with_proxy_->SetDefaultSchemeUrl(proxy_url.c_str(), ec);
            if (ec) {
                throw common::BadArgumentException(ec, "Bad proxy URL", proxy_url, {});
            }

            url_ptr_ = &*url_if_with_proxy_;
        } else {
            url_ptr_ = &easy.get_easy_url();
        }
    }
    MaybeOwnedUrl(const MaybeOwnedUrl&) = delete;
    MaybeOwnedUrl(MaybeOwnedUrl&&) = delete;

    const curl::url& Get() const {
        UASSERT(url_ptr_);
        return *url_ptr_;
    }

private:
    std::optional<curl::url> url_if_with_proxy_;
    const curl::url* url_ptr_{nullptr};
};

// Instance-wide password used to avoid passing unencrypted keys
[[maybe_unused]] const std::string& GetPkeyPassword() {
    static const std::string kPassword = [] {
        CryptoPP::DefaultAutoSeededRNG prng;
        std::string random_bytes;
        random_bytes.resize(32);
        // password should not contain '\0' (cURL only accepts C-style strings)
        while (random_bytes.find('\0') != std::string::npos) {
            static_assert(sizeof(std::string::value_type) == 1, "string does not consist of bytes");
            prng.GenerateBlock(reinterpret_cast<unsigned char*>(random_bytes.data()), random_bytes.size());
        }
        return random_bytes;
    }();
    return kPassword;
}

// Type-dependent implementations to avoid alternate branch instantiation
// (tries to use deleted methods otherwise).
template <typename Easy>
void ModernCaImpl(Easy& easy, crypto::Certificate&& cert) {
    static_assert(Easy::is_set_ca_info_blob_available, "Modern implementation called in legacy env");
    auto cert_pem = cert.GetPemString();
    UINVARIANT(cert_pem, "Could not serialize certificate");
    easy.set_ca_info_blob_copy(*cert_pem);
}

template <typename Easy>
void ModernClientKeyCertImpl(Easy& easy, crypto::PrivateKey&& pkey, crypto::Certificate&& cert) {
    static_assert(
        Easy::is_set_ssl_cert_blob_available && Easy::is_set_ssl_key_blob_available,
        "Modern implementation called in legacy env"
    );
    auto cert_pem = cert.GetPemString();
    UINVARIANT(cert_pem, "Could not serialize certificate");
    easy.set_ssl_cert_blob_copy(*cert_pem);
    easy.set_ssl_cert_type("PEM");
    auto key_pem = pkey.GetPemString(GetPkeyPassword());
    UINVARIANT(key_pem, "Could not serialize private key");
    easy.set_ssl_key_blob_copy(*key_pem);
    easy.set_ssl_key_passwd(GetPkeyPassword());
    easy.set_ssl_key_type("PEM");
}

}  // namespace

size_t RequestState::MessageBodyReadFunction(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* request_state  = static_cast<RequestState*>(userdata);
    const size_t processed_bytes = request_state->bytes_to_read_;
    const size_t room = size * nmemb;
    if(room < 1 || processed_bytes == request_state->message_.size()) {
        return 0;
    }
    const size_t remains = request_state->message_.size() - processed_bytes;
    const size_t len = std::min(remains, room);
    memcpy(ptr, &request_state->message_.at(processed_bytes), len);
    request_state->bytes_to_read_ += len;
    return len;
}

RequestState::RequestState(
    impl::EasyWrapper&& wrapper,
    common::RequestStats&& req_stats,
    const std::shared_ptr<common::DestinationStatistics>& dest_stats,
    clients::dns::Resolver* resolver,
    const tracing::TracingManagerBase& tracing_manager
)
    : easy_(std::move(wrapper)),
      stats_(std::move(req_stats)),
      dest_stats_(dest_stats),
      original_timeout_(kDefaultTimeout),
      remote_timeout_(original_timeout_),
      tracing_manager_{tracing_manager},
      is_cancelled_(false),
      errorbuffer_(),
      resolver_{resolver}
{
    // Libcurl calls sigaction(2)  way too frequently unless this option is used.
    easy().set_no_signal(true);
    easy().set_error_buffer(errorbuffer_.data());



    // set autodecoding
    static const bool kCurlSupportsZstd = [] {
#ifdef CURL_VERSION_ZSTD
        return curl_version_info(curl::native::CURLVERSION_NOW)->features & CURL_VERSION_ZSTD;
#else
        return false;
#endif
    }();

    if (kCurlSupportsZstd) {
        easy().set_accept_encoding("zstd,gzip,deflate,identity");
    } else {
        easy().set_accept_encoding("gzip,deflate,identity");
    }

    // Even if proxy is an empty string we should set it, because empty proxy
    // for CURL disables the use of *_proxy env variables.
    easy().set_proxy("");
}

RequestState::~RequestState() {
    std::error_code ec;
    easy().set_error_buffer(nullptr, ec);
    UASSERT(!ec);
}

void RequestState::follow_redirects(bool follow) {
    easy().set_follow_location(follow);
    easy().set_post_redir(static_cast<long>(follow));
    if (follow) {
        easy().set_max_redirs(kMaxRedirectCount);
    }
}

void RequestState::verify(bool verify) {
    easy().set_ssl_verify_host(verify);
    easy().set_ssl_verify_peer(verify);
}

void RequestState::ca_info(utils::zstring_view file_path) { easy().set_ca_info(file_path.c_str()); }

void RequestState::ca(crypto::Certificate cert) {
    UINVARIANT(cert, "No certificate");
    if constexpr (curl::easy::is_set_ca_info_blob_available) {
        ModernCaImpl(easy(), std::move(cert));
    } else {
        // Legacy non-portable way, broken since 7.87.0
        ca_ = std::move(cert);
        easy().set_ssl_ctx_function(&RequestState::OnCertificateRequest);
        easy().set_ssl_ctx_data(this);
    }
}

void RequestState::crl_file(utils::zstring_view file_path) { easy().set_crl_file(file_path.c_str()); }

void RequestState::client_key_cert(crypto::PrivateKey pkey, crypto::Certificate cert) {
    UINVARIANT(pkey, "No private key");
    UINVARIANT(cert, "No certificate");

    if constexpr (curl::easy::is_set_ssl_cert_blob_available && curl::easy::is_set_ssl_key_blob_available) {
        ModernClientKeyCertImpl(easy(), std::move(pkey), std::move(cert));
    } else {
        // Legacy non-portable way, broken since 7.84.0
        pkey_ = std::move(pkey);
        cert_ = std::move(cert);

        // FIXME: until cURL 7.71 there is no sane way to pass TLS keys from memory.
        // Because of this, we provide our own callback. As a consequence, cURL has
        // no knowledge of the key used and may reuse this connection for a request
        // with a different key or without one.
        // To avoid this until we can upgrade we set the EGD socket option to
        // an unusable certificate-specific value. This option should have no effect
        // on systems targeted by userver anyway but it is accounted when checking
        // cached connection eligibility which is exactly what we need.

        // must be larger than sizeof(sockaddr_un::sun_path)
        static constexpr size_t kCertIdLength = 255;

        // backwards incompatibility
#if OPENSSL_VERSION_NUMBER >= 0x010100000L
        const
#endif
            ASN1_BIT_STRING* cert_sig = nullptr;
        X509_get0_signature(&cert_sig, nullptr, cert_.GetNative());
        UINVARIANT(cert_sig, "Cannot get X509 certificate signature");

        std::string cert_id;
        cert_id.reserve(kCertIdLength);
        utils::encoding::ToHex(
            std::string_view{
                reinterpret_cast<const char*>(cert_sig->data),
                std::min<size_t>(cert_sig->length, kCertIdLength / 2)
            },
            cert_id
        );
        cert_id.resize(kCertIdLength, '=');
        easy().set_egd_socket(cert_id);

        easy().set_ssl_ctx_function(&RequestState::OnCertificateRequest);
        easy().set_ssl_ctx_data(this);
    }
}

void RequestState::http_version(curl::easy::http_version_t version) { easy().set_http_version(version); }

void RequestState::set_timeout(long timeout_ms) {
    original_timeout_ = std::chrono::milliseconds{timeout_ms};
    remote_timeout_ = original_timeout_;
}

void RequestState::retry(short retries, bool on_fails) {
    retry_.retries = retries;
    retry_.current = 1;
    retry_.on_fails = on_fails;
}

void RequestState::unix_socket_path(utils::zstring_view path) { easy().set_unix_socket_path(path); }

void RequestState::connect_to(const http::ConnectTo& connect_to) {
    curl::native::curl_slist* ptr = connect_to.GetUnderlying();
    if (ptr) {
        easy().set_connect_to(ptr);
    }
}

void RequestState::proxy(utils::zstring_view value) {
    proxy_url_ = value;
    easy().set_proxy(value);
}

bool RequestState::IsProxySet() const { return proxy_url_.has_value(); }

void RequestState::proxy_auth_type(curl::easy::proxyauth_t value) { easy().set_proxy_auth(value); }

void RequestState::http_auth_type(
    curl::easy::httpauth_t value,
    bool auth_only,
    utils::zstring_view user,
    utils::zstring_view password
) {
    easy().set_http_auth(value, auth_only);
    easy().set_user(user.c_str());
    easy().set_password(password.c_str());
}

void RequestState::Cancel() {
    // We can not call `retry_.timer.reset();` here because of data race
    is_cancelled_ = true;
    easy().cancel();
}

void RequestState::SetDestinationMetricNameAuto(std::string destination) {
    destination_metric_name_ = std::move(destination);
}

void RequestState::SetDestinationMetricName(const std::string& destination) {
    dest_req_stats_ = dest_stats_->GetStatisticsForDestination(destination);
}

void RequestState::SetTestsuiteConfig(const std::shared_ptr<const http::TestsuiteConfig>& config) {
    testsuite_config_ = config;
}

void RequestState::SetAllowedUrlsExtra(const std::vector<std::string>& urls) { allowed_urls_extra_ = urls; }

void RequestState::DisableReplyDecoding() { easy().set_accept_encoding(nullptr); }

void RequestState::SetCancellationPolicy(common::CancellationPolicy cp) { cancellation_policy_ = cp; }

common::CancellationPolicy RequestState::GetCancellationPolicy() const { return cancellation_policy_; }

void RequestState::SetDeadlinePropagationConfig(const http::DeadlinePropagationConfig& deadline_propagation_config) {
    deadline_propagation_config_ = deadline_propagation_config;
}

curl::native::CURLcode RequestState::OnCertificateRequest(void* /*curl*/, void* sslctx, void* userdata) noexcept {
    auto* ssl = static_cast<SSL_CTX*>(sslctx);
    auto* self = static_cast<RequestState*>(userdata);

    if (!self) {
        return curl::native::CURLcode::CURLE_ABORTED_BY_CALLBACK;
    }

    if (self->ca_) {
        auto* store = SSL_CTX_get_cert_store(ssl);
        if (1 != X509_STORE_add_cert(store, self->ca_.GetNative())) {
            LOG_ERROR() << crypto::FormatSslError("Failed to set up server TLS wrapper: X509_STORE_add_cert");
            return curl::native::CURLcode::CURLE_SSL_CERTPROBLEM;
        }
    }

    if (self->cert_) {
        if (::SSL_CTX_use_certificate(ssl, self->cert_.GetNative()) != 1) {
            LOG_ERROR() << crypto::FormatSslError("Failed to set up server TLS wrapper: SSL_CTX_use_certificate");
            return curl::native::CURLcode::CURLE_SSL_CERTPROBLEM;
        }
    }

    if (self->pkey_) {
        if (::SSL_CTX_use_PrivateKey(ssl, self->pkey_.GetNative()) != 1) {
            LOG_ERROR() << crypto::FormatSslError("Failed to set up server TLS wrapper: SSL_CTX_use_PrivateKey");
            return curl::native::CURLcode::CURLE_SSL_CERTPROBLEM;
        }
    }

    return curl::native::CURLcode::CURLE_OK;
}

std::error_code RequestState::TestsuiteResponseHook(http::Status status_code, const http::Headers& headers, tracing::Span& span) {
    if (status_code == kFakeHttpErrorCode) {
        const auto it = headers.find(std::string_view{"X-Testsuite-Error"});

        if (headers.end() != it) {
            LOG_INFO()
                << "Mockserver faked error of type " << it->second << tracing::impl::LogSpanAsLastNoCurrent{span};

            const auto opt_value = kTestsuiteActions.TryFindByFirst(it->second);
            if (opt_value) {
                return std::error_code{*opt_value};
            }

            utils::AbortWithStacktrace(fmt::format(
                "Unsupported mockserver protocol X-Testsuite-Error header value: {}. "
                "Try to update submodules and recompile project first. If it does "
                "not help please contact testsuite support team.",
                it->second
            ));
        }
    }
    return {};
}

void RequestState::OnCompleted(std::shared_ptr<RequestState> holder, std::error_code err) {
    UASSERT(holder);
    UASSERT(holder->span_storage_);
    auto& span = holder->span_storage_->Get();
    auto& easy = holder->easy();

    LOG_TRACE() << "OnCompleted status_code=" << holder->GetResponseRawStatusCode() << " err=" << err;

    const auto raw_status_code = holder->PreCheckErr(err, span);

    holder->AccountResponse(err);
    const auto sockets = easy.get_num_connects();
    holder->WithRequestStats([sockets](common::RequestStats& stats) { stats.AccountOpenSockets(sockets); });

    span.AddTag(tracing::kAttempts, holder->retry_.current);
    if (holder->deadline_propagation_config_.update_header) {
        span.AddTag("propagated_timeout_ms", holder->remote_timeout_.count());
    }

    if (err) {
        if (easy.rate_limit_error()) {
            // The most probable cause, takes precedence
            err = easy.rate_limit_error();
        }

        holder->middlewares_pipeline_.HookOnError(*holder, err);

        span.AddTag(tracing::kErrorFlag, true);
        span.AddTag(tracing::kErrorMessage, err.message());
        span.AddTag(tracing::kHttpResponseStatusCode, kFakeHttpErrorCode);

        if (holder->errorbuffer_.front()) {
            LOG_DEBUG() << "cURL error details: " << holder->errorbuffer_.data();
        }

        holder->span_storage_.reset();

        holder->SetError(err);
    } else {
        span.AddTag(tracing::kHttpResponseStatusCode, raw_status_code);
        holder->SetResponse(raw_status_code, span);
        holder->span_storage_.reset();
    }
    holder->RequestCompleted();
    // it is unsafe to touch any content of holder after this point!
}
 void RequestState::set_mail_from(std::string&& mail_from) {
    easy().set_mail_from(std::move(mail_from));
 }
void RequestState::set_message(std::string&& message) {
    message_ = std::move(message);
    bytes_to_read_ = 0;
}

void RequestState::OnRetry(std::shared_ptr<RequestState> holder, std::error_code err) {
    UASSERT(holder);
    UASSERT(holder->span_storage_);
    LOG_TRACE() << "RequestImpl::on_retry" << tracing::impl::LogSpanAsLastNoCurrent{holder->span_storage_->Get()};

    // We do not need to retry:
    // - if we got result and HTTP code is good
    // - if we used all attempts
    // - if failed to reach server, and we should not retry on fails
    // - if this request was cancelled
    bool not_need_retry =
        (!err && !holder->ShouldRetryResponse()) || (holder->retry_.current >= holder->retry_.retries) ||
        (err && !holder->retry_.on_fails) || holder->is_cancelled_.load();

    if (!not_need_retry) {
        not_need_retry = !holder->middlewares_pipeline_.HookOnRetry(*holder);
    }

    if (not_need_retry) {
        // finish if no need to retry
        RequestState::OnCompleted(std::move(holder), err);
    } else {
        // calculate backoff before retry
        const auto eb_power = std::clamp(holder->retry_.current - 1, 0, kEBMaxPower);
        const auto backoff = kEBBaseTime * (utils::RandRange(1 << eb_power) + 1);

        holder->UpdateTimeoutFromDeadline(backoff);
        if (holder->remote_timeout_ <= std::chrono::milliseconds::zero()) {
            holder->deadline_expired_ = true;
            RequestState::OnCompleted(std::move(holder), err);
            return;
        }

        holder->AccountResponse(err);

        // increase try
        ++holder->retry_.current;
        holder->easy().mark_retry();

        holder->retry_.timer.emplace(holder->easy().GetThreadControl());

        // call on_retry_timer on timer
        auto& holder_ref = *holder;
        holder_ref.retry_.timer->SingleshotAsync(backoff, [holder = std::move(holder)](std::error_code err) {
            holder->OnRetryTimer(err);
        });
    }
}

void RequestState::OnRetryTimer(std::error_code err) {
    // if there is no error with timer call perform, otherwise finish
    if (!err) {
        PerformRequest([holder = shared_from_this()](std::error_code err) mutable {
            RequestState::OnRetry(std::move(holder), err);
        });
    } else {
        OnCompleted(shared_from_this(), err);
    }
}

void RequestState::SetMiddlewaresList(const std::vector<utils::NotNull<http::MiddlewareBase*>>& middlewares) {
    middlewares_pipeline_ = http::MiddlewaresPipeline(middlewares);
}

void RequestState::SetIncompleteTlsConnectionCloseExpected(bool expect) {
    is_incomplete_tls_connection_close_expected_.store(expect, std::memory_order_release);
}

void RequestState::SetLoggedUrl(std::string url) { log_url_ = std::move(url); }

void RequestState::SetUrlTemplate(std::string url_template) { url_template_ = std::move(url_template); }

void RequestState::SetMethod(http::HttpMethod method) {
    switch (method) {
        case http::HttpMethod::kDelete:
        case http::HttpMethod::kOptions:
            easy().set_custom_request(ToString(method));
            break;
        case http::HttpMethod::kGet:
            easy().set_http_get(true);
            easy().set_custom_request(nullptr);
            break;
        case http::HttpMethod::kHead:
            easy().set_no_body(true);
            easy().set_custom_request(nullptr);
            break;
        // NOTE: set_post makes libcURL to read from stdin if no data is set
        case http::HttpMethod::kPost:
        case http::HttpMethod::kPut:
        case http::HttpMethod::kPatch:
            easy().set_custom_request(ToString(method));
            // ensure a body as we should send Content-Length for this method
            if (!easy().has_post_data()) {
                data({});
            }
            break;
    };
    method_ = method;
}

void RequestState::data(std::string data) {
    if (!data.empty()) {
        easy().add_header(kHeaderExpect, "", curl::easy::EmptyHeaderAction::kDoNotSend);
    }
    easy().set_post_fields(std::move(data));
}

const std::string& RequestState::GetLoggedOriginalUrl() const noexcept {
    // We may want to use original_url if effective_url is not available yet.
    return log_url_ ? *log_url_ : easy().get_original_url();
}

std::string_view RequestState::GetLoggedEffectiveUrl() noexcept {
    // If log_url_ exists, we use log_url_ with a semantic like original_url,
    // instead of effective_url
    return log_url_ ? *log_url_ : easy().get_effective_url();
}

void RequestState::PerformRequest(curl::easy::handler_type handler) {
    UASSERT_MSG(!cert_ || pkey_, "Setting certificate is useless without setting private key");

    ClearResponseBody();

    UpdateTimeoutHeader();

    middlewares_pipeline_.HookPerformRequest(*this);

    if (resolver_ && retry_.current == 1) {
        engine::DetachUnscopedUnsafe(
            engine::AsyncNoSpan([this, holder = shared_from_this(), handler = std::move(handler)]() mutable {
                try {
                    ResolveTargetAddress(*resolver_);
                    easy().async_perform(std::move(handler));
                    return;
                } catch (const clients::dns::ResolverException& ex) {
                    // TODO: should retry - TAXICOMMON-4932
                    SetCurrentException();
                } catch (const common::BaseException& ex) {
                    SetCurrentException();
                }
                span_storage_.reset();
                RequestCompleted();
            })
        );
    } else {
        easy().async_perform(std::move(handler));
    }
}

void RequestState::SetEasyTimeout(std::chrono::milliseconds timeout) {
    UASSERT_MSG(
        timeout >= std::chrono::seconds{0},
        fmt::format("timeout_ms < 0 ({})), uninitialized variable?", timeout)
    );
    easy().set_timeout_ms(timeout.count());
    easy().set_connect_timeout_ms(timeout.count());
}

engine::Deadline RequestState::GetDeadline() const noexcept { return deadline_; }

bool RequestState::IsDeadlineExpired() const noexcept { return deadline_expired_; }

void RequestState::UpdateTimeoutFromDeadline(std::chrono::milliseconds backoff) {
    UASSERT(remote_timeout_ >= std::chrono::milliseconds::zero());
    if (!deadline_.IsReachable()) {
        return;
    }

    const auto timeout_from_deadline = std::clamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline_.TimeLeft() - backoff),
        std::chrono::milliseconds{0},
        original_timeout_
    );

    if (timeout_from_deadline != original_timeout_) {
        remote_timeout_ = timeout_from_deadline;
        timeout_updated_by_deadline_ = true;
        WithRequestStats([](common::RequestStats& stats) { stats.AccountTimeoutUpdatedByDeadline(); });
    }
}

bool RequestState::UpdateTimeoutFromDeadlineAndCheck(std::chrono::milliseconds backoff) {
    UpdateTimeoutFromDeadline(backoff);
    if (remote_timeout_ <= std::chrono::milliseconds::zero()) {
        deadline_expired_ = true;
        HandleDeadlineAlreadyPassed();
        return false;
    }
    return true;
}

void RequestState::UpdateTimeoutHeader() {
    if (!deadline_propagation_config_.update_header) {
        return;
    }

    easy().add_header(
        USERVER_NAMESPACE::http::headers::kXYaTaxiClientTimeoutMs,
        fmt::to_string(remote_timeout_.count()),
        curl::easy::DuplicateHeaderAction::kReplace
    );
}

void RequestState::HandleDeadlineAlreadyPassed() {
    auto& span = span_storage_->Get();
    span.AddTag(tracing::kAttempts, retry_.current - 1);
    span.AddTag(tracing::kErrorFlag, true);
    span.AddTag("propagated_timeout_ms", 0);
    span.AddTag("cancelled_by_deadline", 1);

    WithRequestStats([](common::RequestStats& stats) { stats.AccountCancelledByDeadline(); });

    span_storage_.reset();

    auto exc = PrepareDeadlinePassedException(GetLoggedOriginalUrl(), easy().get_local_stats());

    SetException(exc);
    RequestCompleted();
}

void RequestState::CheckResponseDeadline(std::error_code& err, http::Status status_code) {
    const std::chrono::microseconds attempt_time{easy().get_total_time_usec()};

    if (!deadline_expired_ && timeout_updated_by_deadline_ &&
        (attempt_time >= remote_timeout_ || (!err && IsDeadlineExpiredResponse(status_code))))
    {
        // The most probable cause is IsDeadlineExpiredResponse, case (1).
        // Even if not, the ResponseFuture already has thrown or is preparing
        // to throw a CancelledException, so reflect it here for consistency.
        deadline_expired_ = true;
    }

    if (deadline_expired_) {
        err = std::error_code{curl::errc::EasyErrorCode::kOperationTimedout};
        span_storage_->Get().AddTag("cancelled_by_deadline", 1);
        WithRequestStats([](common::RequestStats& stats) { stats.AccountCancelledByDeadline(); });
    } else if (!err && IsDeadlineExpiredResponse(status_code)) {
        // IsDeadlineExpiredResponse, case (2) happened.
        err = std::error_code{curl::errc::EasyErrorCode::kOperationTimedout};
    }
}

bool RequestState::ShouldRetryResponse() {
    const auto status_code = static_cast<http::Status>(easy().get_response_code());

    if (IsDeadlineExpiredResponse(status_code)) {
        // See IsDeadlineExpiredResponse, case (2).
        return !timeout_updated_by_deadline_ && retry_.on_fails;
    }

    return status_code >= kLeastBadHttpCodeForEB;
}

void RequestState::AccountResponse(std::error_code err) {
    const auto attempts = retry_.current;

    const auto time_to_start = std::chrono::duration_cast<std::chrono::microseconds>(easy().time_to_start());

    WithRequestStats([this, err, attempts, time_to_start](common::RequestStats& stats) {
        stats.StoreTimeToStart(time_to_start);
        if (err) {
            stats.FinishEc(err, attempts);
        } else {
            stats.FinishOk(static_cast<int>(easy().get_response_code()), attempts);
        }
    });
}

std::exception_ptr RequestState::PrepareException(std::error_code err) {
    if (deadline_expired_) {
        return PrepareDeadlinePassedException(GetLoggedEffectiveUrl(), easy().get_local_stats());
    }

    return common::PrepareException(err, GetLoggedEffectiveUrl(), easy().get_local_stats());
}

void RequestState::ThrowDeadlineExpiredException() {
    // This method may be called in parallel with request handling, fetching
    // effective_url_ or local stats is unsafe.
    std::rethrow_exception(PrepareDeadlinePassedException(GetLoggedOriginalUrl(), common::LocalStats{}));
}

void RequestState::ResetDataForNewRequest() {
    SetBaggageHeader(easy());

    ResetResponse();

    is_cancelled_ = false;
    retry_.current = 1;
    remote_timeout_ = original_timeout_;
    deadline_ = server::request::GetTaskInheritedDeadline();
    deadline_expired_ = false;
    timeout_updated_by_deadline_ = false;

    ApplyTestsuiteConfig();

    // Testsuite config might have changed the timeout, so add the tag here.
    // Note: HookPerformRequest can potentially change timeout manually
    // per-attempt. These changes are currently ignored.
    span_storage_->Get().AddTag(tracing::kTimeoutMs, original_timeout_.count());

    // Ignore deadline propagation when setting cURL timeout to avoid closing
    // connections on deadline expiration. The connection will still be closed if
    // the original timeout is exceeded.
    SetEasyTimeout(original_timeout_);

    StartStats();

    ResetRequestCompletion();
}



void RequestState::ApplyTestsuiteConfig() {
    if (!testsuite_config_) {
        return;
    }

    const auto& prefixes = testsuite_config_->allowed_url_prefixes;
    if (!prefixes.empty()) {
        auto url = easy().get_original_url();
        if (!IsPrefix(url, prefixes) && !IsPrefix(url, allowed_urls_extra_)) {
            utils::AbortWithStacktrace(fmt::format(
                "{} forbidden by testsuite config, allowed prefixes={}, "
                "extra prefixes={}",
                url,
                fmt::join(prefixes, ", "),
                fmt::join(allowed_urls_extra_, ", ")
            ));
        }
    }

    const auto& timeout = testsuite_config_->http_request_timeout;
    if (timeout) {
        set_timeout(std::chrono::milliseconds(*timeout).count());
    }

    easy().add_header(kTestsuiteSupportedErrorsKey, GetTestsuiteSupportedErrors());
}

void RequestState::StartNewSpan(utils::impl::SourceLocation location) {
    UINVARIANT(!span_storage_, "Attempt to reuse request while the previous one has not finished");

    std::string span_name;
    if (url_template_.has_value()) {
        span_name = utils::StrCat(ToStringView(method_), " ", url_template_.value());
    } else {
        span_name = utils::StrCat(
            ToStringView(method_),
            " ",
            USERVER_NAMESPACE::http::ExtractHostname(easy().get_original_url())
        );
    }
    span_storage_.emplace(std::move(span_name), location);

    auto& span = span_storage_->Get();

    auto request_editable_instance = GetEditableRequestInstance();

    tracing_manager_->FillRequestWithTracingContext(span, request_editable_instance);
    middlewares_pipeline_.HookCreateSpan(*this, span);
    span.AddTag(tracing::kUrlFull, GetLoggedOriginalUrl());
    span.AddTag(
        tracing::kServerAddress,
        std::string{USERVER_NAMESPACE::http::ExtractHostname(easy().get_original_url())}
    );
    span.AddTag(tracing::kHttpRequestMethod, std::string{ToStringView(method_)});
    if (url_template_.has_value()) {
        span.AddTag(tracing::kHttpUrlTemplate, url_template_.value());
    }
    span.AddTag(tracing::kMaxAttempts, retry_.retries);

    // Span is local to a Request, it is not related to current coroutine
    span.DetachFromCoroStack();
}

void RequestState::StartStats() {
    if (!dest_req_stats_) {
        dest_req_stats_ = dest_stats_->GetStatisticsForDestinationAuto(destination_metric_name_);
    }

    WithRequestStats([](common::RequestStats& stats) { stats.Start(); });
}

template <typename Func>
void RequestState::WithRequestStats(const Func& func) {
    static_assert(std::is_invocable_v<const Func&, common::RequestStats&>);
    func(stats_);
    if (dest_req_stats_) {
        func(*dest_req_stats_);
    }
}

void RequestState::ResolveTargetAddress(clients::dns::Resolver& resolver) {
    const auto deadline = engine::Deadline::FromDuration(remote_timeout_);

    const static std::string kEmptyString;
    const std::string& proxy_url = proxy_url_ ? *proxy_url_ : kEmptyString;
    const MaybeOwnedUrl target{proxy_url, easy()};

    const std::string hostname = target.Get().GetHostPtr().get();

    // CURLOPT_RESOLV hostnames cannot contain colons (as IPv6 addresses do), skip
    if (hostname.find(':') != std::string::npos) {
        return;
    }

    const auto addrs = resolver.Resolve(hostname, deadline);
    auto addr_strings =
        addrs | boost::adaptors::transformed([](const auto& addr) { return addr.PrimaryAddressString(); });

    easy().add_resolve(hostname, target.Get().GetPortPtr().get(), fmt::to_string(fmt::join(addr_strings, ",")));
}

void RequestState::SetTracingManager(const tracing::TracingManagerBase& m) { tracing_manager_ = m; }

http::MiddlewareRequest RequestState::GetEditableRequestInstance() { return http::MiddlewareRequest(*this); }

void RequestState::SetWaitToken(utils::impl::WaitTokenStorageLock&& wait_token) { wait_token_ = std::move(wait_token); }

void RequestState::ResetRequestCompletion() { request_completed_.Reset(); }

void RequestState::RequestCompleted() { request_completed_.Send(); }

void RequestState::WaitForRequestCompletion() {
    if (request_completed_.WaitForEvent()) {
        return;
    }

    UASSERT(engine::current_task::ShouldCancel());
    throw common::CancelException(
        "Wait for the previous HTTP response was aborted due to task cancellation",
        easy().get_local_stats(),
        common::ErrorKind::kCancel
    );
}

}  // namespace clients::http

USERVER_NAMESPACE_END
