#pragma once

/// @file userver/clients/smtp/client.hpp
/// @brief @copybrief clients::smtp::Client

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <userver/clients/smtp/message.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>

USERVER_NAMESPACE_BEGIN

namespace curl {
class easy;
class multi;
class ConnectRateLimiter;
}  // namespace curl

namespace engine::ev {
class ThreadPool;
}  // namespace engine::ev

namespace clients::smtp {

/// How to secure the connection to the SMTP server.
enum class EncryptionType {
    kNone,      ///< plain `smtp://`, no encryption
    kStartTls,  ///< plain `smtp://` upgraded to TLS via the STARTTLS command
    kSmtps,     ///< implicit TLS `smtps://`
};

/// Login/password pair for `AUTH` on the SMTP server.
struct AuthSettings {
    std::string login;
    std::string password;
};

/// @brief Settings of a clients::smtp::Client connection to a single SMTP
/// server.
struct ClientSettings {
    std::string host;
    std::uint16_t port{25};
    EncryptionType encryption{EncryptionType::kNone};

    /// Authentication credentials, if the server requires `AUTH`.
    std::optional<AuthSettings> auth{};

    /// Whether to verify the server TLS certificate (only relevant when
    /// `encryption != kNone`).
    bool verify_tls{true};

    /// Number of libev I/O threads serving the connections.
    std::size_t io_threads{1};
};

/// @ingroup userver_clients
///
/// @brief SMTP client built on top of libcurl's easy/multi interface, the same
/// network engine that backs clients::http::Client.
///
/// The client is bound to a single SMTP server described by ClientSettings and
/// sends MIME messages with Send(). It is safe to use from multiple coroutines
/// concurrently.
///
/// ## Example usage:
///
/// @code
/// clients::smtp::ClientSettings settings;
/// settings.host = "smtp.example.com";
/// settings.port = 587;
/// settings.encryption = clients::smtp::EncryptionType::kStartTls;
/// settings.auth = clients::smtp::AuthSettings{"login", "password"};
///
/// clients::smtp::Client client{settings, fs_task_processor};
///
/// clients::smtp::Message message{
///     {"noreply@example.com", "Example"},
///     {{"user@example.org"}},
///     "Subject",
///     "Hello, world!",
/// };
/// client.Send(message, engine::Deadline::FromDuration(std::chrono::seconds{30}));
/// @endcode
class Client final {
public:
    /// @param settings connection settings of the SMTP server
    /// @param fs_task_processor task processor for the blocking libcurl
    /// initialization (usually the `fs-task-processor`)
    Client(ClientSettings settings, engine::TaskProcessor& fs_task_processor);

    Client(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(const Client&) = delete;
    Client& operator=(Client&&) = delete;

    ~Client();

    /// @brief Sends a single message, blocking the current coroutine until the
    /// transfer completes.
    /// @throws clients::smtp::SendException on any transport or protocol error.
    void Send(const Message& message, engine::Deadline deadline = {});

private:
    std::shared_ptr<curl::easy> GetBoundEasy();

    std::string MakeUrl() const;

    const ClientSettings settings_;
    engine::TaskProcessor& fs_task_processor_;

    std::shared_ptr<curl::ConnectRateLimiter> connect_rate_limiter_;
    std::unique_ptr<engine::ev::ThreadPool> thread_pool_;
    std::vector<std::unique_ptr<curl::multi>> multis_;
    std::shared_ptr<const curl::easy> easy_template_;
};

}  // namespace clients::smtp

USERVER_NAMESPACE_END
