#pragma once

#include <exception>
#include <string>
#include <string_view>
#include <system_error>

#include <userver/clients/common/error.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::http {

/// Base class for HttpClientException and HttpServerException
class HttpException : public common::BaseException {
public:
    HttpException(int code, const common::LocalStats& stats, std::string_view message, common::ErrorKind error_kind);
    ~HttpException() override = default;

    int code() const { return code_; }

private:
    int code_;
};

class HttpClientException : public HttpException {
public:
    HttpClientException(int code, const common::LocalStats& stats);
    HttpClientException(int code, const common::LocalStats& stats, std::string_view message);
    ~HttpClientException() override = default;
};

class HttpServerException : public HttpException {
public:
    HttpServerException(int code, const common::LocalStats& stats);
    HttpServerException(int code, const common::LocalStats& stats, std::string_view message);
    ~HttpServerException() override = default;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
