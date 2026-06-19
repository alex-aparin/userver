#pragma once

/// @file userver/clients/http/response_future.hpp
/// @brief @copybrief clients::http::ResponseFuture

#include <future>
#include <memory>

#include <userver/clients/common/response_future.hpp>
#include <userver/clients/smtp/response.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {

using ResponseFuture = common::ResponseFuture<smtp::Response>;


}  // namespace clients::http

USERVER_NAMESPACE_END
