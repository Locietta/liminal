#include "openai_compaction.h"

#include <iterator>
#include <utility>

#include <lighter/async/runtime/task.h>

namespace liminal::openai::detail {

using lighter::fail;

namespace {

bool selects_local_fallback(const Error &error) {
    if (error.kind != ErrorKind::HTTP_STATUS) {
        return false;
    }
    return error.status == 400 || error.status == 404 || error.status == 405 || error.status == 501;
}

} // namespace

lighter::Task<void, Error> compact_with_retry(const CompactionAttempts &attempts, provider::History &history, usize instruction_count,
                                              std::string body, std::string instructions, usize max_retries,
                                              std::chrono::milliseconds initial_retry_delay) {
    for (usize attempt = 0;; ++attempt) {
        auto outcome = co_await attempts.remote(body);
        if (outcome) {
            provider::History compacted;
            compacted.reserve(instruction_count + 1);
            compacted.insert(compacted.end(), std::make_move_iterator(history.begin()),
                             std::make_move_iterator(history.begin() + instruction_count));
            compacted.push_back({.role = provider::Role::USER});
            for (auto &part : *outcome) {
                compacted.back().parts.push_back(std::move(part));
            }
            history = std::move(compacted);
            co_return;
        }

        auto error = std::move(outcome).error();
        if (selects_local_fallback(error)) {
            co_return co_await attempts.local(history, instructions).or_fail();
        }
        if (attempt >= max_retries || !error.retryable()) {
            co_await fail(std::move(error));
        }
        auto delay = error.retry_after.value_or(initial_retry_delay * (1 << attempt));
        co_await attempts.sleep(delay);
    }
}

} // namespace liminal::openai::detail
