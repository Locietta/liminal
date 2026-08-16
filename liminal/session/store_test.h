#pragma once

#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace liminal::session {

struct SessionRepository;
struct PersistenceQueue;
struct SessionId;

namespace testing {

enum struct StorageEvent {
    AUTHORITATIVE_COMMIT_COMPLETED,
    PUBLICATION_STATE_SNAPSHOTTED,
    CATALOG_INDEXER_BEFORE_REFRESH,
    CATALOG_INDEXER_AFTER_REFRESH,
};

enum struct StorageFailure {
    PUBLICATION_BEFORE_RENAME,
    PUBLICATION_AFTER_RENAME,
    PUBLICATION_AFTER_REOPEN,
};

using StorageHook = std::copyable_function<void(StorageEvent) const>;

struct StorageHookAccess {
    static void set(SessionRepository &repository, StorageHook hook);
    static void fail_once(SessionRepository &repository, StorageFailure failure);
};

struct PersistenceQueueAccess {
    static std::shared_ptr<PersistenceQueue> create_reopening(std::filesystem::path state_root, SessionId id, std::string detail,
                                                              StorageHook hook);
};

inline void set_storage_hook(SessionRepository &repository, StorageHook hook) { StorageHookAccess::set(repository, std::move(hook)); }
inline void fail_storage_once(SessionRepository &repository, StorageFailure failure) { StorageHookAccess::fail_once(repository, failure); }

std::shared_ptr<PersistenceQueue> create_reopening_queue(std::filesystem::path state_root, SessionId id, std::string detail,
                                                         StorageHook hook);

} // namespace testing
} // namespace liminal::session
