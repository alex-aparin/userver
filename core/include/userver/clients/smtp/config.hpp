#pragma once

#include <string>

#include <userver/formats/json_fwd.hpp>
#include <userver/yaml_config/fwd.hpp>

USERVER_NAMESPACE_BEGIN

namespace tracing {
class TracingManagerBase;
}  // namespace tracing

namespace clients::smtp {

// Static config
struct ClientSettings final {
    std::string thread_name_prefix{};
    size_t io_threads{1};
    const tracing::TracingManagerBase* tracing_manager{nullptr};
};

ClientSettings Parse(const yaml_config::YamlConfig& value, formats::parse::To<ClientSettings>);

}  // namespace clients::http

namespace clients::smtp::impl {


// Dynamic config
struct Config final {
    static constexpr std::size_t kDefaultConnectionPoolSize = 10000;

    std::size_t connection_pool_size{kDefaultConnectionPoolSize};
};

}  // namespace clients::http::impl

USERVER_NAMESPACE_END
