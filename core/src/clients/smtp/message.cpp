#include <userver/clients/smtp/message.hpp>

#include <string_view>
#include <utility>

#include <userver/utils/datetime.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

namespace {

constexpr std::string_view kCrlf = "\r\n";

// RFC 5322 date, e.g. "Tue, 19 Jun 2026 12:00:00 +0000".
std::string FormatDate() {
    return utils::datetime::Timestring(utils::datetime::Now(), "UTC", "%a, %d %b %Y %H:%M:%S %z");
}

// Address as it appears in a header field: `"Display Name" <email>`.
std::string FormatHeaderAddress(const Message::Address& address) {
    if (address.display_name.empty()) {
        return address.email;
    }
    return '"' + address.display_name + "\" <" + address.email + '>';
}

std::string FormatHeaderAddressList(const std::vector<Message::Address>& addresses) {
    std::string result;
    for (const auto& address : addresses) {
        if (!result.empty()) {
            result += ", ";
        }
        result += FormatHeaderAddress(address);
    }
    return result;
}

void AppendHeader(std::string& out, std::string_view name, std::string_view value) {
    out += name;
    out += ": ";
    out += value;
    out += kCrlf;
}

// SMTP requires CRLF line endings; bare LF (and lone CR) in the payload are
// normalized. Note that "dot-stuffing" (RFC 5321) is performed by libcurl
// itself, so it must not be done here.
void AppendBodyWithCrlf(std::string& out, std::string_view body) {
    for (const char c : body) {
        if (c == '\r') {
            continue;  // '\n' below emits CRLF
        }
        if (c == '\n') {
            out += kCrlf;
            continue;
        }
        out += c;
    }
}

}  // namespace

Message::Message(Address from, std::vector<Address> to, std::string subject, std::string body)
    : from_(std::move(from)), to_(std::move(to)), subject_(std::move(subject)), body_(std::move(body)) {}

Message& Message::AddRecipient(Address to) {
    to_.push_back(std::move(to));
    return *this;
}

Message& Message::AddCc(Address cc) {
    cc_.push_back(std::move(cc));
    return *this;
}

Message& Message::SetReplyTo(Address reply_to) {
    reply_to_ = {std::move(reply_to)};
    return *this;
}

const std::string& Message::GetSenderAddress() const { return from_.email; }

std::vector<std::string> Message::GetRecipientAddresses() const {
    std::vector<std::string> result;
    result.reserve(to_.size() + cc_.size());
    for (const auto& address : to_) {
        result.push_back(address.email);
    }
    for (const auto& address : cc_) {
        result.push_back(address.email);
    }
    return result;
}

std::string Message::ToString() const {
    std::string result;

    AppendHeader(result, "Date", FormatDate());
    AppendHeader(result, "From", FormatHeaderAddress(from_));
    AppendHeader(result, "To", FormatHeaderAddressList(to_));
    if (!cc_.empty()) {
        AppendHeader(result, "Cc", FormatHeaderAddressList(cc_));
    }
    if (!reply_to_.empty()) {
        AppendHeader(result, "Reply-To", FormatHeaderAddressList(reply_to_));
    }
    AppendHeader(result, "Subject", subject_);
    AppendHeader(result, "MIME-Version", "1.0");
    AppendHeader(result, "Content-Type", "text/plain; charset=UTF-8");
    AppendHeader(result, "Content-Transfer-Encoding", "8bit");

    result += kCrlf;  // header/body separator

    AppendBodyWithCrlf(result, body_);
    result += kCrlf;

    return result;
}

}  // namespace clients::smtp

USERVER_NAMESPACE_END
