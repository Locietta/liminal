#include "sync.h"

#include <contracts>

#include <lighter/async/io/loop.h>

namespace lighter {

void SyncPrimitive::insert(WaitNode *link) {
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
