#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <liminal/error.h>
#include <liminal/session/store.h>

namespace liminal::session {

struct PersistenceStatus {
    bool degraded = false;
    bool catalog_degraded = false;
    usize pending_mutations = 0;
    std::string detail;
    std::string catalog_detail;
};

struct PersistenceQueue {
    using Commit = std::copyable_function<Result<SessionCommitResult>(const SessionDelta &) const>;
    using TestCommit = std::copyable_function<Result<void>(const SessionDelta &) const>;
    using CatalogStatus = std::copyable_function<CatalogRefreshStatus() const>;

    static std::shared_ptr<PersistenceQueue> create(SessionWriter writer);
    static std::shared_ptr<PersistenceQueue> create_unpublished(SessionWriter writer);
    static std::shared_ptr<PersistenceQueue> create_reopening(std::filesystem::path state_root, SessionId id, std::string detail);
    static std::shared_ptr<PersistenceQueue> create_resolving(SessionId id, std::string detail);
    static std::shared_ptr<PersistenceQueue> create_for_test(SessionId id, TestCommit commit);
    static std::shared_ptr<PersistenceQueue> create_unpublished_for_test(SessionId id, TestCommit commit);
    ~PersistenceQueue();

    PersistenceQueue(const PersistenceQueue &) = delete;
    PersistenceQueue &operator=(const PersistenceQueue &) = delete;

    SessionId session_id() const noexcept;
    PersistenceStatus status() const;
    /// Publishes the first complete snapshot through an otherwise unused
    /// queue. This is synchronous so a prepared session cannot become visible
    /// before its initial transaction succeeds.
    Result<void> publish_initial(const SessionDelta &delta);
    Result<void> flush();

private:
    friend struct Session;

    enum struct PublicationState {
        UNPUBLISHED,
        PUBLISHING,
        ACTIVE,
        FAILED,
    };

    PersistenceQueue(SessionId id, Commit commit, PublicationState publication_state, CatalogStatus catalog_status = {});
    void enqueue(SessionDelta delta);
    void mark_degraded(std::string detail);
    void run(std::stop_token stop);
    SessionDelta pending_delta_locked(usize count) const;

    const SessionId id;
    Commit commit;
    CatalogStatus read_catalog_status;
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<SessionDelta> pending;
    u64 enqueued_mutations = 0;
    u64 persisted_mutations = 0;
    u64 failure_generation = 0;
    u64 last_failed_through = 0;
    bool retry_requested = false;
    bool commit_active = false;
    PublicationState publication_state;
    PersistenceStatus current_status;
    std::jthread worker;
};

} // namespace liminal::session
