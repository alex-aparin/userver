#pragma once

/// @file userver/clients/http/response.hpp
/// @brief @copybrief clients::http::Response

#include <string>

#include <userver/clients/common/error.hpp>
#include <userver/clients/common/local_stats.hpp>
#include <userver/clients/smtp/status_code.hpp>
#include <userver/utils/str_icase.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

using Status = USERVER_NAMESPACE::smtp::StatusCode;


/// Class that will be returned for successful request
class Response final {
public:
    Response() = default;

    /// response string
    std::string& sink_string() { return response_; }

    /// body as string
    std::string body() const& { return response_; }
    std::string&& body() && { return std::move(response_); }

    /// body as string_view
    std::string_view body_view() const { return response_; }

    /// status_code
    Status status_code() const;
    /// check status code
    bool IsOk() const { return status_code() == 200; }
    bool IsError() const { return static_cast<uint16_t>(status_code()) >= 400; }

    static void RaiseForStatus(int code, const common::LocalStats& stats);
    static void RaiseForStatus(int code, const common::LocalStats& stats, std::string_view message);

    /// @brief Configuration whether to include the response body in the exception in @ref raise_for_status.
    enum class RaiseIncludeBody : std::uint8_t { kNo = 0, kYes = 1 };

    /// @brief Raise an exception depending on the response status.
    ///        The body of the response may be included in the exception depending on the @param include_body.
    ///
    /// @throws HttpClientException for statuses [400; 500)
    /// @throws HttpServerException for statuses [500; 600)
    void raise_for_status(RaiseIncludeBody include_body = RaiseIncludeBody::kNo) const;

    /// returns statistics on request execution like count of opened sockets, connect time...
    common::LocalStats GetStats() const;

    void SetStats(const common::LocalStats& stats) { stats_ = stats; }
    void SetStatusCode(Status status_code) { status_code_ = status_code; }

private:
    std::string response_;
    Status status_code_{Status::kInvalid};
    common::LocalStats stats_;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
