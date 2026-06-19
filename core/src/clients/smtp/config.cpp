#include <userver/clients/smtp/config.hpp>

#include <string_view>

#include <userver/dynamic_config/value.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/yaml_config/yaml_config.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

ClientSettings Parse(const yaml_config::YamlConfig& value, formats::parse::To<ClientSettings>) {
    ClientSettings result;
    result.thread_name_prefix = value["thread-name-prefix"].As<std::string>(result.thread_name_prefix);
    result.io_threads = value["threads"].As<size_t>(result.io_threads);
    return result;
}

}  // namespace clients::http

USERVER_NAMESPACE_END
