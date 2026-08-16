#include "persistence.h"

#include "paths.h"

#include "catalog.h"
#include "repository.h"
#include "store_test.h"

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

struct LazyWriterState {
    std::mutex mutex;
    std::shared_ptr<SessionWriter> writer;
};

struct LazyWriterCallbacks {
    PersistenceQueue::Commit commit;
    PersistenceQueue::CatalogStatus status;
};

using OpenWriter = std::copyable_function<Result<SessionWriter>() const>;

LazyWriterCallbacks lazy_writer_callbacks(OpenWriter open_writer) {
    auto state = std::make_shared<LazyWriterState>();
    PersistenceQueue::Commit commit = [state,
                                       open_writer = std::move(open_writer)](const SessionDelta &delta) -> Result<SessionCommitResult> {
        std::shared_ptr<SessionWriter> writer;
        {
            std::scoped_lock lock(state->mutex);
            writer = state->writer;
        }
        if (!writer) {
            auto opened = open_writer();
            if (!opened) return lighter::outcome_error(std::move(opened).error());
            writer = std::make_shared<SessionWriter>(*std::move(opened));
            std::scoped_lock lock(state->mutex);
            state->writer = writer;
        }
        return writer->commit(delta);
    };
    PersistenceQueue::CatalogStatus status = [state] {
        std::shared_ptr<SessionWriter> writer;
        {
            std::scoped_lock lock(state->mutex);
            writer = state->writer;
        }
        return writer ? writer->catalog_status() : CatalogRefreshStatus{};
    };
    return {.commit = std::move(commit), .status = std::move(status)};
}

} // namespace

std::shared_ptr<PersistenceQueue> testing::PersistenceQueueAccess::create_reopening(std::filesystem::path state_root, SessionId id,
                                                                                    std::string detail, StorageHook storage_hook) {
    auto callbacks =
        lazy_writer_callbacks([state_root = std::move(state_root), id, storage_hook = std::move(storage_hook)]() -> Result<SessionWriter> {
            auto repository = SessionRepository::open(state_root);
            if (!repository) return lighter::outcome_error(std::move(repository).error());
            if (storage_hook) testing::set_storage_hook(*repository, storage_hook);
            return repository->create(id);
        });
    auto queue = std::shared_ptr<PersistenceQueue>(
        new PersistenceQueue(id, std::move(callbacks.commit), PersistenceQueue::PublicationState::ACTIVE, std::move(callbacks.status)));
    queue->mark_degraded(std::move(detail));
    return queue;
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create(SessionWriter writer) {
    const auto id = writer.session_id();
    auto owned_writer = std::make_shared<SessionWriter>(std::move(writer));
    Commit commit = [owned_writer](const SessionDelta &delta) { return owned_writer->commit(delta); };
    CatalogStatus status = [owned_writer] { return owned_writer->catalog_status(); };
    return std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(commit), PublicationState::ACTIVE, std::move(status)));
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_unpublished(SessionWriter writer) {
    const auto id = writer.session_id();
    auto owned_writer = std::make_shared<SessionWriter>(std::move(writer));
    Commit commit = [owned_writer](const SessionDelta &delta) { return owned_writer->commit(delta); };
    CatalogStatus status = [owned_writer] { return owned_writer->catalog_status(); };
    return std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(commit), PublicationState::UNPUBLISHED, std::move(status)));
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_for_test(SessionId id, TestCommit commit) {
    Commit adapted = [commit = std::move(commit)](const SessionDelta &delta) -> Result<SessionCommitResult> {
        auto result = commit(delta);
        if (!result) return lighter::outcome_error(std::move(result).error());
        return SessionCommitResult{};
    };
    return std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(adapted), PublicationState::ACTIVE));
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_unpublished_for_test(SessionId id, TestCommit commit) {
    Commit adapted = [commit = std::move(commit)](const SessionDelta &delta) -> Result<SessionCommitResult> {
        auto result = commit(delta);
        if (!result) return lighter::outcome_error(std::move(result).error());
        return SessionCommitResult{};
    };
    return std::shared_ptr<PersistenceQueue>(new PersistenceQueue(id, std::move(adapted), PublicationState::UNPUBLISHED));
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_reopening(std::filesystem::path state_root, SessionId id, std::string detail) {
    return testing::PersistenceQueueAccess::create_reopening(std::move(state_root), id, std::move(detail), {});
}

std::shared_ptr<PersistenceQueue> PersistenceQueue::create_resolving(SessionId id, std::string detail) {
    auto callbacks = lazy_writer_callbacks([id]() -> Result<SessionWriter> {
        auto path = state_root_path();
        if (!path) return lighter::outcome_error(std::move(path).error());
        auto repository = SessionRepository::open(*path);
        if (!repository) return lighter::outcome_error(std::move(repository).error());
        return repository->create(id);
    });
    auto queue = std::shared_ptr<PersistenceQueue>(
        new PersistenceQueue(id, std::move(callbacks.commit), PublicationState::ACTIVE, std::move(callbacks.status)));
    queue->mark_degraded(std::move(detail));
    return queue;
}

std::shared_ptr<PersistenceQueue> testing::create_reopening_queue(std::filesystem::path state_root, SessionId id, std::string detail,
                                                                  StorageHook hook) {
    return PersistenceQueueAccess::create_reopening(std::move(state_root), id, std::move(detail), std::move(hook));
}

PersistenceQueue::PersistenceQueue(SessionId id, Commit commit, PublicationState publication_state, CatalogStatus catalog_status)
    : id(id), commit(std::move(commit)), read_catalog_status(std::move(catalog_status)), publication_state(publication_state),
      worker([this](std::stop_token stop) { run(stop); }) {}

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
    PersistenceStatus result;
    {
        std::scoped_lock lock(mutex);
        result = current_status;
    }
    if (read_catalog_status) {
        const auto catalog = read_catalog_status();
        result.catalog_degraded = catalog.degraded;
        result.catalog_detail = catalog.detail;
    }
    return result;
}

Result<void> PersistenceQueue::publish_initial(const SessionDelta &delta) {
    {
        std::scoped_lock lock(mutex);
        if (publication_state != PublicationState::UNPUBLISHED || !pending.empty() || enqueued_mutations != 0 || persisted_mutations != 0 ||
            commit_active) {
            return lighter::outcome_error(Error::protocol("initial publication requires an unused persistence queue"));
        }
        publication_state = PublicationState::PUBLISHING;
        commit_active = true;
    }
    auto published = commit(delta);
    {
        std::scoped_lock lock(mutex);
        commit_active = false;
        if (published) {
            publication_state = PublicationState::ACTIVE;
            current_status = {.pending_mutations = pending.size()};
            if (published->catalog_degradation) {
                current_status.catalog_degraded = true;
                current_status.catalog_detail = *published->catalog_degradation;
            }
            if (!pending.empty()) retry_requested = true;
        } else {
            publication_state = PublicationState::FAILED;
            current_status.degraded = true;
            current_status.pending_mutations = pending.size();
            current_status.detail = published.error().message();
        }
    }
    changed.notify_all();
    if (!published) return lighter::outcome_error(std::move(published).error());
    return {};
}

void PersistenceQueue::mark_degraded(std::string detail) {
    std::scoped_lock lock(mutex);
    current_status.degraded = true;
    current_status.detail = std::move(detail);
}

Result<void> PersistenceQueue::flush() {
    std::unique_lock lock(mutex);
    if (publication_state == PublicationState::UNPUBLISHED) {
        return lighter::outcome_error(Error::protocol("initial session publication has not started"));
    }
    if (publication_state == PublicationState::PUBLISHING) {
        changed.wait(lock, [this] { return publication_state != PublicationState::PUBLISHING; });
    }
    if (publication_state == PublicationState::FAILED) {
        return lighter::outcome_error(
            Error::storage(current_status.detail.empty() ? "initial session publication failed" : current_status.detail));
    }
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
            changed.wait(lock, stop, [this] {
                return publication_state == PublicationState::ACTIVE && !commit_active && !pending.empty() && retry_requested;
            });
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
                if (result->catalog_degradation) {
                    current_status.catalog_degraded = true;
                    current_status.catalog_detail = *result->catalog_degradation;
                }
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
