#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <lighter/types.hpp>

namespace lighter {

class RingBuffer {
public:
    /// contract: pre(cap > 0)
    explicit RingBuffer(usize cap = 64 * 1024) : storage(std::make_unique_for_overwrite<char[]>(cap)), capacity(cap) {
        assert(cap > 0 && "RingBuffer capacity must be greater than zero");
    }

    usize readable_bytes() const { return size; }

    usize writable_bytes() const { return capacity - size; }

    usize read(char *dest, usize len);

    std::pair<const char *, usize> get_read_ptr() const;
    void advance_read(usize len);

    std::pair<char *, usize> get_write_ptr();
    void advance_write(usize len);

private:
    std::unique_ptr<char[]> storage;
    usize capacity;
    usize head = 0;
    usize tail = 0;
    usize size = 0;
};

} // namespace lighter
