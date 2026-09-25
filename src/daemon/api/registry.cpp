#include <braurser/daemon/api/registry.hpp>
#include <algorithm>
#include <exception>

namespace braurser::daemon::api {

void HandlerRegistry::dispatch(const protocol::Request& request, Sender sender) const {
    dispatch(request, {}, std::move(sender));
}

void HandlerRegistry::dispatch(const protocol::Request& request, RequestContext context, Sender sender) const {
    const auto descriptor = std::find_if(protocol::kMethods.begin(), protocol::kMethods.end(),
        [&](const auto& method) { return method.name == request.method; });
    if (descriptor == protocol::kMethods.end() || !handlers_.contains(request.method)) {
        sender(protocol::ErrorObject{protocol::kMethodNotFound, "error.generic_title"});
        return;
    }
    if (!descriptor->validate_params(request.params)) {
        sender(protocol::ErrorObject{protocol::kInvalidParams, "error.generic_title"});
        return;
    }
    auto delivered = std::make_shared<bool>(false);
    auto once = [sender = std::move(sender), delivered](auto outcome) {
        if (*delivered) return;
        *delivered = true;
        sender(std::move(outcome));
    };
    try {
        handlers_.at(request.method)(request.params, context, once);
    } catch (const std::exception&) {
        once(protocol::ErrorObject{protocol::kInternalError, "error.generic_title"});
    } catch (...) {
        once(protocol::ErrorObject{protocol::kInternalError, "error.generic_title"});
    }
}

}
