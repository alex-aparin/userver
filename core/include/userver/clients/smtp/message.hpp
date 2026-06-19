#pragma once

/// @file userver/clients/smtp/message.hpp
/// @brief @copybrief clients::smtp::Message

#include <string>
#include <vector>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

/// @ingroup userver_clients
///
/// @brief A simple e-mail message that is serialized into a MIME
/// (RFC 5322 / RFC 2045) representation by Message::ToString().
///
/// The message always uses a single `text/plain; charset=UTF-8` body, which is
/// enough for the majority of transactional e-mails.
class Message final {
public:
    /// An e-mail address with an optional human readable display name.
    struct Address {
        std::string email;
        std::string display_name{};
    };

    Message(Address from, std::vector<Address> to, std::string subject, std::string body);

    /// Adds a `To:` recipient.
    Message& AddRecipient(Address to);

    /// Adds a `Cc:` recipient.
    Message& AddCc(Address cc);

    /// Sets the optional `Reply-To:` header.
    Message& SetReplyTo(Address reply_to);

    /// Envelope sender address used for the `MAIL FROM` SMTP command.
    const std::string& GetSenderAddress() const;

    /// Envelope recipient addresses (both `To` and `Cc`) used for the
    /// `RCPT TO` SMTP commands.
    std::vector<std::string> GetRecipientAddresses() const;

    /// Serializes the message into a MIME representation with CRLF line
    /// endings, ready to be fed to an SMTP server.
    std::string ToString() const;

private:
    Address from_;
    std::vector<Address> to_;
    std::vector<Address> cc_;
    std::vector<Address> reply_to_;
    std::string subject_;
    std::string body_;
};

}  // namespace clients::smtp

USERVER_NAMESPACE_END
