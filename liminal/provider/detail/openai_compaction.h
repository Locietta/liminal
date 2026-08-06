#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/async/runtime/task.h>
#include <lighter/types.hpp>

#include <liminal/error.h>
#include <liminal/provider/history.h>

namespace liminal::openai::detail {

struct CompactionAttempts {
    std::copyable_function<lighter::Task<std::vector<provider::OpaquePart>, Error>(const std::string &body) const> remote;
    std::copyable_function<lighter::Task<void, Error>(provider::History &history, std::string_view instructions) const> local;
    std::copyable_function<lighter::Task<>(std::chrono::milliseconds delay) const> sleep;
};

lighter::Task<void, Error> compact_with_retry(const CompactionAttempts &attempts, provider::History &history, usize instruction_count,
                                              std::string body, std::string instructions, usize max_retries,
                                              std::chrono::milliseconds initial_retry_delay);

} // namespace liminal::openai::detail
