#include <userver/utest/using_namespace_userver.hpp>

#include <userver/clients/smtp/message.hpp>
#include <userver/utest/utest.hpp>

USERVER_NAMESPACE_BEGIN

namespace {
namespace cs = clients::smtp;

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(SmtpMessage, EnvelopeAddresses) {
    cs::Message message{
        {"from@example.com", "Sender"},
        {{"to@example.org", "Receiver"}},
        "Subject",
        "Body",
    };
    message.AddCc({"cc@example.net"});

    EXPECT_EQ(message.GetSenderAddress(), "from@example.com");
    EXPECT_EQ(
        message.GetRecipientAddresses(), (std::vector<std::string>{"to@example.org", "cc@example.net"})
    );
}

TEST(SmtpMessage, MimeHeaders) {
    cs::Message message{
        {"from@example.com", "Sender"},
        {{"to@example.org", "Receiver"}},
        "Hello",
        "Body text",
    };
    message.AddCc({"cc@example.net"}).SetReplyTo({"reply@example.com", "Reply Bot"});

    const auto mime = message.ToString();

    EXPECT_TRUE(Contains(mime, "From: \"Sender\" <from@example.com>\r\n"));
    EXPECT_TRUE(Contains(mime, "To: \"Receiver\" <to@example.org>\r\n"));
    EXPECT_TRUE(Contains(mime, "Cc: cc@example.net\r\n"));
    EXPECT_TRUE(Contains(mime, "Reply-To: \"Reply Bot\" <reply@example.com>\r\n"));
    EXPECT_TRUE(Contains(mime, "Subject: Hello\r\n"));
    EXPECT_TRUE(Contains(mime, "MIME-Version: 1.0\r\n"));
    EXPECT_TRUE(Contains(mime, "Content-Type: text/plain; charset=UTF-8\r\n"));
    EXPECT_TRUE(Contains(mime, "Date: "));

    // Headers are separated from the body by an empty line.
    EXPECT_TRUE(Contains(mime, "\r\n\r\nBody text\r\n"));
}

TEST(SmtpMessage, CrlfNormalization) {
    cs::Message message{
        {"from@example.com"},
        {{"to@example.org"}},
        "Subject",
        "first line\n.dot command\nlast",
    };

    const auto mime = message.ToString();

    // Bare LF is normalized to CRLF; the leading dot is left intact because
    // libcurl performs SMTP dot-stuffing itself.
    EXPECT_TRUE(Contains(mime, "first line\r\n.dot command\r\nlast\r\n"));
}

USERVER_NAMESPACE_END
