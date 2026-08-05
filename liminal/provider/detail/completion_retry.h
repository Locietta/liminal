#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include <lighter/async/runtime/task.h>
#include <lighter/types.hpp>

#include "liminal/error.h"
#include "liminal/provider/common.h"
#include "liminal/provider/history.h"

namespace liminal::provider::detail {

/// Injectable boundary around one provider streaming attempt and retry delay.
struct CompletionAttempts {
    std::copyable_function<lighter::Task<TurnResponse, Error>(const std::string &body, const StreamCallbacks &callbacks, bool &text_emitted)
                               const>
        stream;
    std::copyable_function<lighter::Task<>(std::chrono::milliseconds delay) const> sleep;
};

lighter::Task<TurnResponse, Error> complete_with_retry(const CompletionAttempts &attempts, std::string body,
                                                       const StreamCallbacks &callbacks, usize max_retries,
                                                       std::chrono::milliseconds initial_retry_delay);

} // namespace liminal::provider::detail
