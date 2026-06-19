#include <userver/clients/smtp/component.hpp>

#include <stdexcept>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

namespace {

clients::smtp::EncryptionType ParseEncryption(const std::string& value) {
    if (value == "none") {
        return clients::smtp::EncryptionType::kNone;
    }
    if (value == "starttls") {
        return clients::smtp::EncryptionType::kStartTls;
    }
    if (value == "smtps") {
        return clients::smtp::EncryptionType::kSmtps;
    }
    throw std::runtime_error("Unknown SMTP encryption type '" + value + "', expected none/starttls/smtps");
}

clients::smtp::ClientSettings MakeSettings(const ComponentConfig& config) {
    clients::smtp::ClientSettings settings;
    settings.host = config["host"].As<std::string>();
    settings.port = config["port"].As<std::uint16_t>(25);
    settings.encryption = ParseEncryption(config["encryption"].As<std::string>("none"));
    settings.verify_tls = config["verify-tls"].As<bool>(true);
    settings.io_threads = config["io-threads"].As<std::size_t>(1);

    const auto auth = config["auth"];
    if (!auth.IsMissing()) {
        settings.auth = clients::smtp::AuthSettings{
            auth["login"].As<std::string>(),
            auth["password"].As<std::string>(),
        };
    }
    return settings;
}

}  // namespace

SmtpClient::SmtpClient(const ComponentConfig& config, const ComponentContext& context)
    : ComponentBase(config, context), client_(MakeSettings(config), GetFsTaskProcessor(config, context)) {}

clients::smtp::Client& SmtpClient::GetClient() { return client_; }

yaml_config::Schema SmtpClient::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: Component that manages a clients::smtp::Client.
additionalProperties: false
properties:
    host:
        type: string
        description: SMTP server host
    port:
        type: integer
        description: SMTP server port
        default: 25
    encryption:
        type: string
        description: connection encryption, one of none/starttls/smtps
        default: none
    verify-tls:
        type: boolean
        description: whether to verify the server TLS certificate
        default: true
    io-threads:
        type: integer
        description: number of libev I/O threads serving the connections
        default: 1
    auth:
        type: object
        description: credentials for SMTP AUTH (enables authentication when present)
        additionalProperties: false
        properties:
            login:
                type: string
                description: login for SMTP AUTH
            password:
                type: string
                description: password for SMTP AUTH
)");
}

}  // namespace components

USERVER_NAMESPACE_END
