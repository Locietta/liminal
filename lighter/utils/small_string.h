#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#include <lighter/scalar_types.hpp>
#include <lighter/utils/small_vector.h>
#include <lighter/utils/string_ref.h>

namespace lighter {

/// A SmallVector<char> with string-like convenience methods.
/// All string operations delegate to StringRef.
template <u32 InlineCapacity>
class SmallString : public SmallVector<char, InlineCapacity> {
    using base = SmallVector<char, InlineCapacity>;

public:
    /// Default ctor - Initialize to empty.
    constexpr SmallString() = default;

    /// Initialize from a string_view.
    constexpr SmallString(std::string_view s) : base(s) {}

    /// Initialize by concatenating a list of string_views.
    constexpr SmallString(std::initializer_list<std::string_view> refs) : base() {
        this->append(refs);
    }

    /// Adopt a pre-allocated buffer as a SmallString.
    /// The buffer must have been allocated with mem::allocate<char>.
    [[nodiscard]] constexpr static SmallString from_raw_parts(char *data,
                                                              usize count,
                                                              usize capacity) {
        SmallString result;
        if (data != nullptr && capacity > 0) {
            result.adopt_allocation(data, count, capacity);
        }
        return result;
    }

    // --- String Assignment ---

    using base::assign;

    /// Assign from a string_view.
    constexpr void assign(std::string_view rhs) {
        base::assign(rhs);
    }

    /// Assign from a list of string_views.
    constexpr void assign(std::initializer_list<std::string_view> refs) {
        this->clear();
        append(refs);
    }

    // --- String Concatenation ---

    using base::append;

    /// Append from a string_view.
    constexpr void append(std::string_view rhs) {
        base::append(rhs);
    }

    /// Append from a list of string_views.
    constexpr void append(std::initializer_list<std::string_view> refs) {
        usize current_size = this->size();
        usize size_needed = current_size;
        for (const std::string_view &ref : refs) {
            size_needed += ref.size();
        }
        this->resize_for_overwrite(size_needed);
        for (const std::string_view &ref : refs) {
            std::copy(ref.begin(), ref.end(), this->begin() + current_size);
            current_size += ref.size();
        }
    }

    // --- Conversion ---

    /// Get a StringRef view of this string.
    [[nodiscard]] constexpr StringRef ref() const {
        return StringRef(this->data(), this->size());
    }

    /// Get a null-terminated C string.
    constexpr const char *c_str() {
        this->push_back(0);
        this->pop_back();
        return this->data();
    }

    /// Implicit conversion to StringRef.
    constexpr operator StringRef() const {
        return ref();
    }

    /// Implicit conversion to string_view.
    constexpr operator std::string_view() const {
        return std::string_view(this->data(), this->size());
    }

    /// Explicit conversion to std::string.
    constexpr explicit operator std::string() const {
        return std::string(this->data(), this->size());
    }

    // --- Operators ---

    constexpr SmallString &operator=(std::string_view rhs) {
        this->assign(rhs);
        return *this;
    }

    constexpr SmallString &operator+=(std::string_view rhs) {
        this->append(rhs);
        return *this;
    }

    constexpr SmallString &operator+=(char c) {
        this->push_back(c);
        return *this;
    }

    friend constexpr bool operator==(const SmallString &lhs, std::string_view rhs) {
        return lhs.ref() == rhs;
    }

    friend constexpr auto operator<=>(const SmallString &lhs, std::string_view rhs) {
        return lhs.ref().compare(rhs) <=> 0;
    }
};

} // namespace lighter
