#pragma once

/// @file userver/clients/smtp/component.hpp
/// @brief @copybrief components::SmptuClient

#include <userver/clients/smtp/client.hpp>
#include <userver/components/component_base.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

/// @ingroup userver_components
///
/// @brief Component that manages @ref clients::http::ClientWithMiddlewares.
///
/// Reuses @ref clients::http::ClientCore from @ref components::HttpClientCore and applies
/// sequence of @ref clients::http::MiddlewareBase to the request.
///
/// Returned references to @ref clients::http::Client live for a lifetime of the
/// component and are safe for concurrent use.
///
/// ## Static options of components::HttpClient :
/// @include{doc} scripts/docs/en/components_schema/core/src/clients/http/component.md
///
/// Options inherited from @ref components::ComponentBase :
/// @include{doc} scripts/docs/en/components_schema/core/src/components/impl/component_base.md
///
/// ## Static configuration example:
///
/// @snippet components/common_component_list_test.cpp  Sample http client component config
class SmtpClient final : public ComponentBase {
public:
    /// @ingroup userver_component_names
    /// @brief The default name of components::SmtpClient component
    static constexpr std::string_view kName = "smtp-client";

    SmtpClient(const ComponentConfig&, const ComponentContext&);

    clients::smtp::Client& GetSmtpClient();

    static yaml_config::Schema GetStaticConfigSchema();

private:
    clients::smtp::Client smtp_client_;
};

template <>
inline constexpr bool kHasValidate<SmtpClient> = true;

template <>
inline constexpr auto kConfigFileMode<SmtpClient> = ConfigFileMode::kNotRequired;

}  // namespace components

USERVER_NAMESPACE_END
