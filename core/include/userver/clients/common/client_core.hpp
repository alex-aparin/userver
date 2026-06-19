#pragma once

/// @file userver/clients/http/client_core.hpp
/// @brief @copybrief clients::http::ClientCore

#include <memory>

#include <userver/yaml_config/fwd.hpp>

USERVER_NAMESPACE_BEGIN

namespace curl {
class easy;
}  // namespace curl


namespace clients::common {
namespace impl {
class EasyWrapper;
}  // namespace impl

/// @ingroup userver_clients
///
/// @brief HTTP client that returns a HTTP request builder from
/// CreateRequest().
///
/// Usually retrieved from @ref components::HttpClientCore component.
///
/// ## Example usage:
///
/// @snippet clients/http/client_test.cpp  Sample HTTP Client usage
class ClientBase{
public:

    virtual ~ClientBase(){}

    // Functions for EasyWrapper that must be noexcept, as they are called from
    // the EasyWrapper destructor.
    friend class impl::EasyWrapper;
    virtual void IncPending() noexcept  = 0;
    virtual void DecPending() noexcept  = 0;
    virtual void PushIdleEasy(std::shared_ptr<curl::easy>&& easy)  = 0;
};

}  // namespace clients::http

USERVER_NAMESPACE_END
