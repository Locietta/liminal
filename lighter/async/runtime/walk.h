#pragma once

#include <set>

#include <lighter/async/runtime/node.h>
#include <lighter/async/runtime/sync.h>

namespace lighter {

const inline AsyncNode *get_parent(const AsyncNode &node) {
    using NK = AsyncNode::NodeKind;
    switch (node.kind) {
        case NK::TASK: return static_cast<const TaskFrame &>(node).get_parent();
        case NK::MUTEX_WAITER:
        case NK::EVENT_WAITER: return static_cast<const WaitNode &>(node).get_parent();
        case NK::WHEN_ALL:
        case NK::WHEN_ANY:
        case NK::TASK_GROUP: return static_cast<const AggregateOp &>(node).get_parent();
        case NK::SYSTEM_IO: return static_cast<const IoOp &>(node).get_parent();
    }
    return nullptr;
}

const inline SyncPrimitive *get_resource(const AsyncNode &node) {
    using NK = AsyncNode::NodeKind;
    switch (node.kind) {
        case NK::MUTEX_WAITER:
        case NK::EVENT_WAITER: return static_cast<const WaitNode &>(node).get_resource();
        default: return nullptr;
    }
}

template <typename Derived>
class AsyncVisitor {
public:
    void walk_node(const AsyncNode &node) {
        if (!visited_.insert(&node).second) {
            return;
        }

        using NK = AsyncNode::NodeKind;
        switch (node.kind) {
            case NK::TASK: {
                auto &task = static_cast<const TaskFrame &>(node);
                if (!derived().visit_task(task))
                    return;
                if (auto *child = task.get_child()) {
                    derived().visit_edge(&task, child);
                    walk_node(*child);
                }
                break;
            }

            case NK::MUTEX_WAITER:
            case NK::EVENT_WAITER: {
                auto &wn = static_cast<const WaitNode &>(node);
                if (!derived().visit_wait_node(wn))
                    return;
                if (auto *res = wn.get_resource()) {
                    derived().visit_edge(&wn, res);
                    walk_sync(*res);
                }
                break;
            }

            case NK::WHEN_ALL:
            case NK::WHEN_ANY:
            case NK::TASK_GROUP: {
                auto &agg = static_cast<const AggregateOp &>(node);
                if (!derived().visit_aggregate(agg))
                    return;
                for (auto *child : agg.get_children()) {
                    if (child) {
                        derived().visit_edge(&agg, child);
                        walk_node(*child);
                    }
                }
                break;
            }

            case NK::SYSTEM_IO: {
                derived().visit_io(static_cast<const IoOp &>(node));
                break;
            }
        }
    }

    void walk_sync(const SyncPrimitive &resource) {
        if (!visited_.insert(&resource).second) {
            return;
        }

        if (!derived().visit_sync(resource))
            return;
        for (auto *w = resource.get_head(); w; w = w->get_next()) {
            derived().visit_edge(&resource, w);
            walk_node(*w);
        }
    }

    void reset() {
        visited_.clear();
    }

protected:
    bool visit_task(const TaskFrame &) {
        return true;
    }

    bool visit_wait_node(const WaitNode &) {
        return true;
    }

    bool visit_aggregate(const AggregateOp &) {
        return true;
    }

    bool visit_io(const IoOp &) {
        return true;
    }

    bool visit_sync(const SyncPrimitive &) {
        return true;
    }

    void visit_edge(const void *, const void *) {}

private:
    Derived &derived() {
        return static_cast<Derived &>(*this);
    }

    std::set<const void *> visited_;
};

} // namespace lighter
