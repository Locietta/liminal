#pragma once

#include <string>

namespace lighter {

class AsyncNode;

template <typename T, typename E, typename C>
class Task;

std::string dump_dot(const AsyncNode &root);

template <typename T, typename E, typename C>
std::string dump_dot(Task<T, E, C> &t) {
    return dump_dot(*t.operator->());
}

} // namespace lighter
