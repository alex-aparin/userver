#pragma once

/// @file userver/clients/smtp/component.hpp
/// @brief @copybrief components::SmtpClient

#include <optional>

#include <userver/clients/smtp/client.hpp>
#include <userver/components/component_base.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

/// @ingroup userver_components
///
/// @brief Component that manages a clients::smtp::Client bound to a single SMTP
/// server.
///
/// The reference returned by GetClient() lives for the lifetime of the
/// component and is safe for concurrent use.
///
/// ## Static options:
/// Name          | Description                                            | Default value
/// ------------- | ------------------------------------------------------ | -------------
/// host          | SMTP server host                                       | --
/// port          | SMTP server port                                       | 25
/// encryption    | one of `none`, `starttls`, `smtps`                     | none
/// verify-tls    | verify the server TLS certificate                      | true
/// io-threads    | number of libev I/O threads                            | 1
/// auth.login    | login for SMTP `AUTH` (enables authentication)         | --
/// auth.password | password for SMTP `AUTH`                               | --
///
/// @warning Storing credentials in the static config is discouraged for
/// production; prefer providing them via storages::secdist.
class SmtpClient final : public ComponentBase {
public:
    /// @ingroup userver_component_names
    /// @brief The default name of components::SmtpClient component
    static constexpr std::string_view kName = "smtp-client";

    SmtpClient(const ComponentConfig&, const ComponentContext&);

    /// @brief Returns the managed SMTP client.
    clients::smtp::Client& GetClient();

    static yaml_config::Schema GetStaticConfigSchema();

private:
    clients::smtp::Client client_;
};

template <>
inline constexpr bool kHasValidate<SmtpClient> = true;

}  // namespace components

USERVER_NAMESPACE_END
