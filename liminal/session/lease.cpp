#include "lease.h"

namespace liminal::session {

SessionLease::SessionLease(std::shared_ptr<State> state) : state(std::move(state)) {}
SessionLease::~SessionLease() = default;
SessionLease::SessionLease(SessionLease &&) noexcept = default;
SessionLease &SessionLease::operator=(SessionLease &&) noexcept = default;

} // namespace liminal::session
