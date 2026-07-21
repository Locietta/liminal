#include "sync.h"

#include <cassert>

#include <lighter/async/io/loop.h>

namespace lighter {

void SyncPrimitive::insert(WaitNode *link) {
    assert(link && "insert: null WaitNode");
    assert(link->resource == nullptr && "insert: WaitNode already linked");
    assert(link->prev == nullptr && link->next == nullptr && "insert: WaitNode has links");

    link->resource = this;

    if (tail) {
        tail->next = link;
        link->prev = tail;
        tail = link;
    } else {
        head = link;
        tail = link;
    }
}

void SyncPrimitive::remove(WaitNode *link) {
    assert(link && "remove: null WaitNode");
    assert(link->resource == this && "remove: WaitNode not owned by resource");

    if (link->prev) {
        link->prev->next = link->next;
    } else {
        head = link->next;
    }

    if (link->next) {
        link->next->prev = link->prev;
    } else {
        tail = link->prev;
    }

    link->prev = nullptr;
    link->next = nullptr;
    link->resource = nullptr;
}

bool SyncPrimitive::resume_waiter(WaitNode &link) noexcept {
    auto *awaiting = link.parent;
    assert(awaiting && "resume_waiter: waiter has no parent");
    assert(EventLoop::has_current() && "resume_waiter: no Event loop on this thread");
    if (awaiting->is_cancelled()) {
        link.parent = nullptr;
        return false;
    }
    link.state = AsyncNode::FINISHED;
    EventLoop::current().defer_resume(*awaiting);
    return true;
}

bool SyncPrimitive::cancel_waiter(WaitNode &link) noexcept {
    auto *awaiting = link.parent;
    assert(awaiting && "cancel_waiter: waiter has no parent");
    assert(EventLoop::has_current() && "cancel_waiter: no Event loop on this thread");
    if (awaiting->is_cancelled()) {
        link.parent = nullptr;
        return false;
    }
    link.state = AsyncNode::CANCELLED;
    link.policy = static_cast<AsyncNode::Policy>(link.policy | AsyncNode::INTERCEPT_CANCEL);
    EventLoop::current().defer_resume(*awaiting);
    return true;
}

} // namespace lighter
