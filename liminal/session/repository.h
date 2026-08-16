#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <liminal/error.h>
#include <liminal/session/catalog.h>
#include <liminal/session/store.h>

namespace liminal::session {

namespace testing {
struct StorageHookAccess;
} // namespace testing

struct CatalogReconciliation {
    usize repaired = 0;
    usize busy = 0;
    std::vector<std::string> warnings;
};

enum struct RepositoryOpenMode {
    INITIALIZE_CATALOG,
    DEFER_CATALOG_REBUILD,
};

struct SessionRepository {
    struct State;

    static Result<SessionRepository> open(std::filesystem::path state_root,
                                          RepositoryOpenMode mode = RepositoryOpenMode::INITIALIZE_CATALOG);
    static Result<SessionRepository> repair_catalog(std::filesystem::path state_root);

    const std::filesystem::path &root() const noexcept;
    Result<SessionCatalog> catalog() const;
    const std::vector<std::string> &warnings() const noexcept;
    Result<SessionWriter> create(SessionId id) const;
    Result<SessionWriter> stage(SessionId id, const SessionDelta &initial) const;
    Result<SessionWriter> acquire(SessionId id) const;
    Result<bool> remove_catalog_hint_if_authority_absent(SessionId id) const;
    Result<SessionId> resolve_exact(std::string_view text) const;
    Result<CatalogReconciliation> reconcile_pending() const;
    Result<CatalogReconciliation> rebuild_catalog() const;

private:
    friend struct testing::StorageHookAccess;
    static Result<SessionRepository> open_authority(std::filesystem::path state_root);
    static Result<void> attach_catalog(SessionRepository &repository, SessionCatalog catalog);
    static Result<SessionRepository> open_with_catalog(std::filesystem::path state_root, SessionCatalog catalog);
    explicit SessionRepository(std::shared_ptr<State> state) : state(std::move(state)) {}
    std::shared_ptr<State> state;
};

} // namespace liminal::session
