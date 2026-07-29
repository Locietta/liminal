#include "ringbuffer.h"

#include <algorithm>
#include <contracts>
#include <cstring>

#include <lighter/types.hpp>

namespace lighter {

usize RingBuffer::read(char *dest, usize len) {
    const usize to_read = std::min(len, size);
    if (to_read == 0) {
        return 0;
    }

    const usize first_chunk = std::min(to_read, capacity - head);
    std::memcpy(dest, storage.get() + head, first_chunk);

    const usize remaining = to_read - first_chunk;
    if (remaining > 0) {
        std::memcpy(dest + first_chunk, storage.get(), remaining);
    }

    head = (head + to_read) % capacity;
    size -= to_read;
    return to_read;
}

std::pair<const char *, usize> RingBuffer::get_read_ptr() const {
    if (size == 0) {
        return {nullptr, 0};
    }

    // When the buffer is full, head == tail but size > 0. Using `>=` here
    // would yield contiguous = 0, causing read_chunk() to return an empty
    // span and the caller to spin forever. Use strict `>` so the full case
    // falls through to the else branch (capacity - head), which is correct.
    usize contiguous = 0;
    if (tail > head) {
        contiguous = tail - head;
    } else {
        contiguous = capacity - head;
    }

    contract_assert(contiguous > 0);
    return {storage.get() + head, contiguous};
}

void RingBuffer::advance_read(usize len) {
    if (len > size) {
        len = size;
    }

    head = (head + len) % capacity;
    size -= len;
}

std::pair<char *, usize> RingBuffer::get_write_ptr() {
    const usize writable = writable_bytes();
    usize contiguous = 0;
    if (writable == 0) {
        contiguous = 0;
    } else if (tail >= head) {
        contiguous = std::min(writable, capacity - tail);
    } else {
        contiguous = head - tail;
    }

    return {storage.get() + tail, contiguous};
}

void RingBuffer::advance_write(usize len) {
    const usize writable = writable_bytes();
    if (len > writable) {
        len = writable;
    }

    tail = (tail + len) % capacity;
    size += len;
}

} // namespace lighter
