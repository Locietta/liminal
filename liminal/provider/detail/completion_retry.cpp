#include "completion_retry.h"

#include <utility>

#include <lighter/async/runtime/task.h>

namespace liminal::provider::detail {

using lighter::fail;

lighter::Task<TurnResponse, Error> complete_with_retry(const CompletionAttempts &attempts, std::string body,
                                                       const StreamCallbacks &callbacks, usize max_retries,
                                                       std::chrono::milliseconds initial_retry_delay) {
    bool text_emitted = false;
    for (usize attempt = 0;; ++attempt) {
        auto outcome = co_await attempts.stream(body, callbacks, text_emitted);
        if (outcome) {
            co_return *std::move(outcome);
        }
        auto error = std::move(outcome).error();
        if (attempt >= max_retries || !error.retryable() || text_emitted) {
            co_await fail(std::move(error));
        }

        auto delay = error.retry_after.value_or(initial_retry_delay * (1 << attempt));
        co_await attempts.sleep(delay);
    }
}

} // namespace liminal::provider::detail
