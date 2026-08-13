#include "persistence.h"

#include "paths.h"

#include <array>
#include <chrono>
#include <optional>
#include <random>
#include <utility>

#include <lighter/async/vocab/outcome.h>

namespace liminal::session {

namespace {

using namespace std::chrono_literals;

constexpr std::array k_retry_delays{100ms, 300ms, 1000ms};
constexpr auto k_background_retry_delay = 5s;

} // namespace

std::shared_ptr<PersistenceQueue> PersistenceQueue::create(SessionWriter writer) {
    const auto id = writer.session_id();
    auto owned_writer = std::make_shared<SessionWriter>(std::move(writer));
    Commit commit = [owned_writer](const SessionDelta &delta) { return owned_writer->commit(delta); };
    return std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(commit)));
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_for_test(SessionId id, Commit commit) {
    return std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(commit)));
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_reopening(std::filesystem::path database_path, SessionId id,
                                                                     std::string detail) {
    struct ReopeningStore {
        std::filesystem::path path;
        SessionId id;
        std::optional<SessionWriter> writer;
    };
    auto reopening = std::make_shared<ReopeningStore>(ReopeningStore{.path = std::move(database_path), .id = id});
    Commit commit = [reopening](const SessionDelta &delta) -> Result<void> {
        if (!reopening->writer) {
            auto opened = Store::open(reopening->path);
            if (!opened) return lighter::outcome_error(std::move(opened).error());
            auto writer = opened->lease(reopening->id);
            if (!writer) return lighter::outcome_error(std::move(writer).error());
            reopening->writer = *std::move(writer);
        }
        return reopening->writer->commit(delta);
    };
    auto queue = std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(commit)));
    queue->mark_degraded(std::move(detail));
    return queue;
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_resolving(SessionId id, std::string detail) {
    struct ResolvingStore {
        SessionId id;
        std::optional<SessionWriter> writer;
    };
    auto resolving = std::make_shared<ResolvingStore>(ResolvingStore{.id = id});
    Commit commit = [resolving](const SessionDelta &delta) -> Result<void> {
        if (!resolving->writer) {
            auto path = state_database_path();
            if (!path) return lighter::outcome_error(std::move(path).error());
            auto opened = Store::open(*path);
            if (!opened) return lighter::outcome_error(std::move(opened).error());
            auto writer = opened->lease(resolving->id);
            if (!writer) return lighter::outcome_error(std::move(writer).error());
            resolving->writer = *std::move(writer);
        }
        return resolving->writer->commit(delta);
    };
    auto queue = std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(commit)));
    queue->mark_degraded(std::move(detail));
    return queue;
}

PersistenceQueue::PersistenceQueue(SessionId id, Commit commit)
    : id(id), commit(std::move(commit)), worker([this](std::stop_token stop) { run(stop); }) {}

PersistenceQueue::~PersistenceQueue() {
    worker.request_stop();
    changed.notify_all();
}

void PersistenceQueue::enqueue(SessionDelta delta) {
    {
        std::scoped_lock lock(mutex);
        pending.push_back(std::move(delta));
        ++enqueued_mutations;
        current_status.pending_mutations = pending.size();
        retry_requested = true;
    }
    changed.notify_all();
}

SessionId PersistenceQueue::session_id() const noexcept { return id; }

PersistenceStatus PersistenceQueue::status() const {
    std::scoped_lock lock(mutex);
    return current_status;
}

void PersistenceQueue::mark_degraded(std::string detail) {
    std::scoped_lock lock(mutex);
    current_status.degraded = true;
    current_status.detail = std::move(detail);
}

Result<void> PersistenceQueue::flush() {
    std::unique_lock lock(mutex);
    if (pending.empty()) return {};
    const auto target = enqueued_mutations;
    const auto observed_failure = failure_generation;
    retry_requested = true;
    changed.notify_all();
    changed.wait(lock, [this, target, observed_failure] {
        return persisted_mutations >= target || (failure_generation > observed_failure && last_failed_through >= target && !commit_active);
    });
    if (persisted_mutations >= target) return {};
    return lighter::outcome_error(Error::storage(current_status.detail.empty() ? "session has an unsaved tail" : current_status.detail));
}

SessionDelta PersistenceQueue::pending_delta_locked(usize count) const {
    auto delta = pending[count - 1];
    delta.entries.clear();
    for (usize index = 0; index < count; ++index) {
        delta.entries.insert(delta.entries.end(), pending[index].entries.begin(), pending[index].entries.end());
    }
    return delta;
}

void PersistenceQueue::run(std::stop_token stop) {
    std::minstd_rand random(std::random_device{}());
    usize failures = 0;
    while (!stop.stop_requested()) {
        SessionDelta delta;
        usize count = 0;
        u64 attempt_through = 0;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, stop, [this] { return !pending.empty() && retry_requested; });
            if (stop.stop_requested()) return;
            retry_requested = false;
            count = pending.size();
            attempt_through = persisted_mutations + count;
            delta = pending_delta_locked(count);
            commit_active = true;
        }

        auto result = commit(delta);
        {
            std::scoped_lock lock(mutex);
            commit_active = false;
            if (result) {
                for (usize index = 0; index < count; ++index) pending.pop_front();
                persisted_mutations += count;
                current_status = {.pending_mutations = pending.size()};
                failures = 0;
                if (!pending.empty()) retry_requested = true;
                changed.notify_all();
                continue;
            }
            ++failures;
            ++failure_generation;
            last_failed_through = attempt_through;
            current_status.degraded = true;
            current_status.pending_mutations = pending.size();
            current_status.detail = result.error().message();
            changed.notify_all();
        }

        const auto base_delay = failures <= k_retry_delays.size() ? k_retry_delays[failures - 1] : k_background_retry_delay;
        const auto jitter = std::chrono::milliseconds(random() % 31);
        std::unique_lock lock(mutex);
        changed.wait_for(lock, stop, base_delay + jitter, [this] { return retry_requested; });
        if (stop.stop_requested()) return;
        retry_requested = true;
    }
}

} // namespace liminal::session
