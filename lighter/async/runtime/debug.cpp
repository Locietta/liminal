#include "debug.h"

#include <format>
#include <string>
#include <string_view>

#include <lighter/async/runtime/walk.h>

namespace lighter {

static std::string_view async_kind_name(AsyncNode::NodeKind k) {
    switch (k) {
        case AsyncNode::NodeKind::TASK: return "Task";
        case AsyncNode::NodeKind::MUTEX_WAITER: return "MutexWaiter";
        case AsyncNode::NodeKind::EVENT_WAITER: return "EventWaiter";
        case AsyncNode::NodeKind::WHEN_ALL: return "WhenAll";
        case AsyncNode::NodeKind::WHEN_ANY: return "WhenAny";
        case AsyncNode::NodeKind::TASK_GROUP: return "TaskGroup";
        case AsyncNode::NodeKind::SYSTEM_IO: return "SystemIO";
    }
    return "Unknown";
}

static std::string_view state_name(AsyncNode::State s) {
    switch (s) {
        case AsyncNode::PENDING: return "PENDING";
        case AsyncNode::RUNNING: return "RUNNING";
        case AsyncNode::CANCELLED: return "CANCELLED";
        case AsyncNode::FINISHED: return "FINISHED";
        case AsyncNode::FAILED: return "FAILED";
    }
    return "Unknown";
}

static std::string_view sync_kind_name(SyncPrimitive::Kind k) {
    switch (k) {
        case SyncPrimitive::Kind::MUTEX: return "Mutex";
        case SyncPrimitive::Kind::EVENT: return "Event";
        case SyncPrimitive::Kind::SEMAPHORE: return "Semaphore";
        case SyncPrimitive::Kind::CONDITION_VARIABLE: return "ConditionVariable";
    }
    return "Unknown";
}

static std::string node_id(const void *node) {
    return std::format("n{:x}", reinterpret_cast<std::uintptr_t>(node));
}

static std::string_view basename(const char *path) {
    if (!path || path[0] == '\0') {
        return {};
    }
    std::string_view sv(path);
    auto pos = sv.find_last_of(R"(/\)");
    return pos != std::string_view::npos ? sv.substr(pos + 1) : sv;
}

static void emit_async_node(const AsyncNode *node, std::string &out) {
    auto file = basename(node->location.file_name());
    std::string label;
    if (!file.empty()) {
        label = std::format(R"({}
{}
{}:{})",
                            async_kind_name(node->kind),
                            state_name(node->state),
                            file,
                            node->location.line());
    } else {
        label = std::format(R"({}
{})",
                            async_kind_name(node->kind),
                            state_name(node->state));
    }

    std::string_view shape = "box";
    std::string_view color = "white";

    if (node->is_task_frame()) {
        switch (node->state) {
            case AsyncNode::RUNNING: color = R"("#90EE90")"; break;
            case AsyncNode::FINISHED: color = R"("#D3D3D3")"; break;
            case AsyncNode::CANCELLED: color = R"("#FFB6C1")"; break;
            case AsyncNode::FAILED: color = R"("#FFA07A")"; break;
            default: break;
        }
    } else if (node->is_aggregate_op()) {
        shape = "diamond";
        color = R"("#D8BFD8")";
    } else if (node->kind == AsyncNode::NodeKind::SYSTEM_IO) {
        color = R"("#FFFFE0")";
    } else if (node->is_wait_node()) {
        color = R"("#FFDAB9")";
    }

    std::format_to(std::back_inserter(out),
                   R"(  {} [label="{}", shape={}, style=filled, fillcolor={}];
)",
                   node_id(node),
                   label,
                   shape,
                   color);
}

static void emit_sync_node(const SyncPrimitive *resource, std::string &out) {
    auto file = basename(resource->location.file_name());
    std::string label;
    if (!file.empty()) {
        label = std::format(R"({}
{}:{})",
                            sync_kind_name(resource->kind),
                            file,
                            resource->location.line());
    } else {
        label = std::format("{}", sync_kind_name(resource->kind));
    }

    std::format_to(std::back_inserter(out),
                   R"(  {} [label="{}", shape=ellipse, style=filled, fillcolor="{}"];
)",
                   node_id(resource),
                   label,
                   "#ADD8E6");
}

struct DotEmitter : AsyncVisitor<DotEmitter> {
    std::string out;

    bool visit_task(const TaskFrame &node) {
        emit_async_node(&node, out);
        return true;
    }

    bool visit_wait_node(const WaitNode &node) {
        emit_async_node(&node, out);
        return true;
    }

    bool visit_aggregate(const AggregateOp &node) {
        emit_async_node(&node, out);
        return true;
    }

    bool visit_io(const IoOp &node) {
        emit_async_node(&node, out);
        return true;
    }

    bool visit_sync(const SyncPrimitive &resource) {
        emit_sync_node(&resource, out);
        return true;
    }

    void visit_edge(const void *from, const void *to) {
        std::format_to(std::back_inserter(out),
                       R"(  {} -> {};
)",
                       node_id(from),
                       node_id(to));
    }
};

std::string dump_dot(const AsyncNode &root) {
    const auto *node = &root;
    while (auto *p = get_parent(*node)) {
        node = p;
    }

    DotEmitter emitter;
    emitter.out += R"(digraph async_graph {
)";
    emitter.out += R"(  rankdir=TB;
)";
    emitter.out += R"(  node [fontname="Helvetica", fontsize=10];
)";

    emitter.walk_node(*node);

    emitter.out += R"(}
)";
    return std::move(emitter.out);
}

} // namespace lighter
