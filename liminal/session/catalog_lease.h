#pragma once

#include <filesystem>
#include <memory>
#include <utility>

#include <liminal/error.h>

namespace liminal::session::detail {

struct CatalogLease {
    struct State;
    ~CatalogLease();
    CatalogLease(CatalogLease &&) noexcept;
    CatalogLease &operator=(CatalogLease &&) noexcept;
    CatalogLease(const CatalogLease &) = delete;
    CatalogLease &operator=(const CatalogLease &) = delete;

private:
    friend Result<CatalogLease> acquire_catalog_lease(const std::filesystem::path &, bool);
    friend Result<CatalogLease> acquire_catalog_initialization_lease(const std::filesystem::path &);
    explicit CatalogLease(std::shared_ptr<State> state) : state(std::move(state)) {}
    std::shared_ptr<State> state;
};

Result<CatalogLease> acquire_catalog_lease(const std::filesystem::path &state_root, bool exclusive);
Result<CatalogLease> acquire_catalog_initialization_lease(const std::filesystem::path &state_root);

} // namespace liminal::session::detail
