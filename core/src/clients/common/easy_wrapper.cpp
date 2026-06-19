#include <clients/common/easy_wrapper.hpp>

#include <userver/clients/common/client_core.hpp>
#include <userver/utils/assert.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::common::impl {

EasyWrapper::EasyWrapper(std::shared_ptr<curl::easy>&& easy, ClientBase& client)
    : easy_(std::move(easy)),
      client_(client)
{
    client_.IncPending();
}

EasyWrapper::EasyWrapper(EasyWrapper&&) noexcept = default;

EasyWrapper::~EasyWrapper() {
    if (easy_) {
        client_.PushIdleEasy(std::move(easy_));
    }
}

curl::easy& EasyWrapper::Easy() { return *easy_; }

const curl::easy& EasyWrapper::Easy() const { return *easy_; }

}  // namespace clients::http::impl

USERVER_NAMESPACE_END
