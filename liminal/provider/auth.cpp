#include "auth.h"

#include <string>
#include <utility>

namespace liminal::provider {

AuthResolver api_key_auth(std::string api_key, bool bearer) {
    return [api_key = std::move(api_key), bearer]() -> lighter::Task<ResolvedAuth, Error> {
        ResolvedAuth auth;
        if (bearer) {
            auth.bearer_token = api_key;
        } else {
            auth.api_key = api_key;
        }
        co_return auth;
    };
}

} // namespace liminal::provider
