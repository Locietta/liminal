#pragma once

#include <memory>

namespace lighter {

template <typename T>
struct DestroyHandle {
    void operator()(T *ptr) const noexcept { T::destroy(ptr); }
};

template <typename T>
using UniqueHandle = std::unique_ptr<T, DestroyHandle<T>>;

} // namespace lighter
