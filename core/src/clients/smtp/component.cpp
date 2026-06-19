#include <userver/clients/smtp/component.hpp>

#include <boost/range/adaptor/transformed.hpp>

#include <userver/clients/smtp/client.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/utils/algo.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#ifndef ARCADIA_ROOT
#include "generated/src/clients/smtp/component.yaml.hpp"  // Y_IGNORE
#endif

USERVER_NAMESPACE_BEGIN

namespace components {

namespace {

static clients::smtp::ClientSettings GetClientSettings(
    const ComponentConfig& component_config,
    const ComponentContext& context
) {
    clients::smtp::ClientSettings settings;
    settings = component_config.As<clients::smtp::ClientSettings>();
    return settings;
}

}

SmtpClient::SmtpClient(const ComponentConfig& component_config, const ComponentContext& context)
    : ComponentBase(component_config, context),
      smtp_client_(utils::impl::InternalTag{}, GetClientSettings(component_config, context), GetFsTaskProcessor(component_config, context)){

}

clients::smtp::Client& SmtpClient::GetSmtpClient() { return smtp_client_; }

yaml_config::Schema SmtpClient::GetStaticConfigSchema() {
    return yaml_config::MergeSchemasFromResource<ComponentBase>("src/clients/smtp/component.yaml");
}

}  // namespace components

USERVER_NAMESPACE_END
