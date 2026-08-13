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
    usize pending_mutations = 0;
    std::string detail;
};

struct PersistenceQueue : std::enable_shared_from_this<PersistenceQueue> {
    using Commit = std::copyable_function<Result<void>(const SessionDelta &) const>;

    static std::shared_ptr<PersistenceQueue> create(SessionWriter writer);
    static std::shared_ptr<PersistenceQueue> create_reopening(std::filesystem::path database_path, SessionId id, std::string detail);
    static std::shared_ptr<PersistenceQueue> create_resolving(SessionId id, std::string detail);
    static std::shared_ptr<PersistenceQueue> create_for_test(Commit commit);
    ~PersistenceQueue();

    PersistenceQueue(const PersistenceQueue &) = delete;
    PersistenceQueue &operator=(const PersistenceQueue &) = delete;

    void enqueue(SessionDelta delta);
    PersistenceStatus status() const;
    Result<void> flush();
    void mark_degraded(std::string detail);

private:
    explicit PersistenceQueue(Commit commit);
    void run(std::stop_token stop);
    SessionDelta pending_delta_locked(usize count) const;

    Commit commit;
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<SessionDelta> pending;
    u64 enqueued_mutations = 0;
    u64 persisted_mutations = 0;
    u64 failure_generation = 0;
    u64 last_failed_through = 0;
    bool retry_requested = false;
    bool commit_active = false;
    PersistenceStatus current_status;
    std::jthread worker;
};

} // namespace liminal::session
