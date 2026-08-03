#pragma once

#include <functional>
#include <string>
#include <vector>

#include <lighter/async/runtime/task.h>
#include <lighter/http/common.h>

#include "liminal/error.h"

namespace liminal::provider {

struct ResolvedAuth {
    std::string bearer_token;
    std::string api_key;
    std::vector<lighter::http::Header> headers;
};

using AuthResolver = std::copyable_function<lighter::Task<ResolvedAuth, Error>() const>;

AuthResolver api_key_auth(std::string api_key, bool bearer);

} // namespace liminal::provider
