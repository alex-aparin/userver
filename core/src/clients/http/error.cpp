#include <userver/clients/http/error.hpp>

#include <fmt/format.h>

#include <curl-ev/error_code.hpp>
#include <userver/utils/assert.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::http {

HttpException::HttpException(int code, const common::LocalStats& stats, std::string_view message, common::ErrorKind error_kind)
    : common::BaseException(
          fmt::format("Raise for status exception, code = {}{}{}", code, message.empty() ? "" : ": ", message),
          stats,
          error_kind
      ),
      code_(code)
{}

HttpClientException::HttpClientException(int code, const common::LocalStats& stats)
    : HttpClientException(code, stats, {})
{}

HttpClientException::HttpClientException(int code, const common::LocalStats& stats, std::string_view message)
    : HttpException(code, stats, message, common::ErrorKind::kClient)
{}

HttpServerException::HttpServerException(int code, const common::LocalStats& stats)
    : HttpServerException(code, stats, {})
{}

HttpServerException::HttpServerException(int code, const common::LocalStats& stats, std::string_view message)
    : HttpException(code, stats, message, common::ErrorKind::kServer)
{}

}  // namespace clients::http

USERVER_NAMESPACE_END
