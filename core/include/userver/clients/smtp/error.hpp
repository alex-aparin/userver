#pragma once

/// @file userver/clients/smtp/error.hpp
/// @brief Exceptions thrown by the SMTP client

#include <stdexcept>
#include <system_error>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

/// @brief Base class for all SMTP client exceptions.
class BaseException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Thrown when sending a message fails (connection, TLS, authentication
/// or any other transport/protocol error reported by libcurl).
class SendException : public BaseException {
public:
    SendException(const std::string& message, std::error_code ec)
        : BaseException(message), ec_(ec) {}

    /// The underlying libcurl error code.
    std::error_code error_code() const noexcept { return ec_; }

private:
    std::error_code ec_;
};

}  // namespace clients::smtp

USERVER_NAMESPACE_END
