#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

#include <lighter/async/async.h>
#include <lighter/async/runtime/task_group.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

/// Cancels its own group before its first suspension. The frame is live on
/// the stack while the cancel cascade reaches it, so the cascade must defer
/// to the next suspension point rather than finalize (and destroy) it here.
Task<> cancel_group_from_first_segment(TaskGroup<> &group, bool &resumed_after_cancel) {
    group.cancel();
    co_await yield();
    resumed_after_cancel = true;
}

/// Same, but one level down: the spawned child is suspended on this Task,
/// which is what makes the child look idle to the cascade.
Task<> cancel_group_from_awaited_first_segment(TaskGroup<> &group, bool &resumed_after_cancel) {
    group.cancel();
    co_await yield();
    resumed_after_cancel = true;
}

Task<> await_then_finish(TaskGroup<> &group, bool &resumed_after_cancel, bool &parent_finished) {
    co_await cancel_group_from_awaited_first_segment(group, resumed_after_cancel);
    parent_finished = true;
}

/// A spawned child that cancels the group in its first synchronous segment
/// must be cancelled at its next suspension, and join() must still settle.
Task<> spawned_child_cancels_group(bool &ok) {
    bool resumed_after_cancel = false;
    TaskGroup group(EventLoop::current());
    require(group.spawn(cancel_group_from_first_segment(group, resumed_after_cancel)), "spawn must accept the child");
    co_await group.join();
    ok = !resumed_after_cancel;
}

/// A Task awaited by a spawned child cancels the group in its first segment.
/// The cascade reaches it through the child's link and must not finalize it
/// while it is executing.
Task<> awaited_task_cancels_group(bool &ok) {
    bool resumed_after_cancel = false;
    bool parent_finished = false;
    TaskGroup group(EventLoop::current());
    require(group.spawn(await_then_finish(group, resumed_after_cancel, parent_finished)), "spawn must accept the child");
    co_await group.join();
    ok = !resumed_after_cancel && !parent_finished;
}

/// A group whose children all finish normally joins with every child complete.
Task<> children_complete(bool &ok) {
    i32 finished = 0;
    auto child = [&finished]() -> Task<> {
        co_await yield();
        ++finished;
    };
    TaskGroup group(EventLoop::current());
    require(group.spawn(child()) && group.spawn(child()) && group.spawn(child()), "spawn must accept the children");
    co_await group.join();
    ok = finished == 3;
}

i32 run_all() {
    EventLoop loop;

    bool complete_ok = false;
    bool spawned_ok = false;
    bool awaited_ok = false;

    loop.schedule(children_complete(complete_ok));
    loop.schedule(spawned_child_cancels_group(spawned_ok));
    loop.schedule(awaited_task_cancels_group(awaited_ok));
    loop.run();

    require(complete_ok, "every spawned child must run to completion before join() settles");
    require(spawned_ok, "a child cancelling its group in its first segment must be cancelled at its next suspension");
    require(awaited_ok, "a Task awaited by a child must survive a cancel cascade issued from its first segment");
    return 0;
}

} // namespace

i32 main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
