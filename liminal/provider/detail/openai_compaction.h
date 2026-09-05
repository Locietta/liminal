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
#include <liminal/provider/provider.h>

namespace liminal::openai::detail {

struct CompactionAttempts {
    std::copyable_function<lighter::Task<std::vector<provider::OpaquePart>, Error>(const std::string &body) const> remote;
    std::copyable_function<lighter::Task<void, Error>(provider::History &history, provider::CompactRequest request) const> local;
    std::copyable_function<lighter::Task<>(std::chrono::milliseconds delay) const> sleep;
};

lighter::Task<void, Error> compact_with_retry(const CompactionAttempts &attempts, provider::History &history, usize instruction_count,
                                              std::string body, provider::CompactRequest request, usize max_retries,
                                              std::chrono::milliseconds initial_retry_delay);

} // namespace liminal::openai::detail
