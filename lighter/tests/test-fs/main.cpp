#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <lighter/async/io/fs.h>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>

namespace {

using namespace lighter;

static_assert(std::is_move_constructible_v<fs::DirHandle>);
static_assert(std::is_move_assignable_v<fs::DirHandle>);

void require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

Task<void, Error> exercise_directory_handle(bool &moved_from_empty, bool &assigned_from_empty, bool &closed) {
    auto source = co_await fs::opendir(".").or_fail();
    fs::DirHandle destination(std::move(source));
    moved_from_empty = !source.valid() && destination.valid();

    auto entries = co_await fs::readdir(destination).or_fail();
    (void) entries;
    fs::DirHandle assigned;
    assigned = std::move(destination);
    assigned_from_empty = !destination.valid() && assigned.valid();
    co_await fs::closedir(assigned).or_fail();
    closed = !assigned.valid();
}

int run_all() {
    EventLoop loop;
    bool moved_from_empty = false;
    bool assigned_from_empty = false;
    bool closed = false;
    auto task = exercise_directory_handle(moved_from_empty, assigned_from_empty, closed);
    loop.schedule(task);
    loop.run();

    auto outcome = task.result();
    require(outcome.has_value(), "directory handle lifecycle failed");
    require(moved_from_empty, "move construction must transfer exactly one native handle");
    require(assigned_from_empty, "move assignment must transfer into an empty destination");
    require(closed, "closedir must invalidate the native handle");
    return 0;
}

} // namespace

int main() {
    try {
        return run_all();
    } catch (const std::exception &error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
