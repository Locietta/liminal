#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <lighter/scalar_types.hpp>

namespace lighter {

/// A string_view extension with convenient string manipulation methods.
/// Inherits from std::string_view and adds operations like split, trim,
/// take/drop, consume, count, find_if, etc. All methods are constexpr.
class StringRef : public std::string_view {
    using base = std::string_view;

    constexpr static i32 compare_memory(const char *lhs, const char *rhs, usize length) {
        if (length == 0) {
            return 0;
        }
        for (usize i = 0; i < length; ++i) {
            auto l = static_cast<u8>(lhs[i]);
            auto r = static_cast<u8>(rhs[i]);
            if (l != r) {
                return l < r ? -1 : 1;
            }
        }
        return 0;
    }

    constexpr static char to_lower(char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    constexpr static char to_upper(char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }

    constexpr static i32 ascii_strncasecmp(StringRef lhs, StringRef rhs, usize length) {
        for (usize i = 0; i < length; ++i) {
            auto l = static_cast<u8>(to_lower(lhs[i]));
            auto r = static_cast<u8>(to_lower(rhs[i]));
            if (l != r) {
                return l < r ? -1 : 1;
            }
        }
        return 0;
    }

public:
    using base::base;
    using base::npos;

    constexpr StringRef() = default;

    constexpr StringRef(std::string_view sv) : base(sv) {}

    constexpr StringRef(const std::string &s) : base(s) {}

    // rejects temporary std::string to avoid dangling references
    constexpr StringRef(std::string &&) = delete;
    constexpr StringRef(std::string const &&) = delete;
    constexpr StringRef &operator=(std::string &&) = delete;
    constexpr StringRef &operator=(std::string const &&) = delete;

    // --- Comparison ---

    /// Compare two strings; result is negative, zero, or positive.
    [[nodiscard]] constexpr i32 compare(StringRef rhs) const {
        if (i32 res = compare_memory(data(), rhs.data(), std::min(size(), rhs.size()))) {
            return res;
        }
        if (size() == rhs.size()) {
            return 0;
        }
        return size() < rhs.size() ? -1 : 1;
    }

    /// Compare two strings, ignoring case.
    [[nodiscard]] constexpr i32 compare_insensitive(StringRef rhs) const {
        auto min = std::min(size(), rhs.size());
        if (i32 res = ascii_strncasecmp(*this, rhs, min)) {
            return res;
        }
        if (size() == rhs.size()) {
            return 0;
        }
        return size() < rhs.size() ? -1 : 1;
    }

    /// Check for string equality, ignoring case.
    [[nodiscard]] constexpr bool equals_insensitive(StringRef rhs) const {
        return size() == rhs.size() && compare_insensitive(rhs) == 0;
    }

    // --- Predicates ---

    /// Check if this string starts with the given prefix, ignoring case.
    [[nodiscard]] constexpr bool starts_with_insensitive(StringRef prefix) const {
        return size() >= prefix.size() && ascii_strncasecmp(*this, prefix, prefix.size()) == 0;
    }

    /// Check if this string ends with the given suffix, ignoring case.
    [[nodiscard]] constexpr bool ends_with_insensitive(StringRef suffix) const {
        return size() >= suffix.size() &&
               ascii_strncasecmp(drop_front(size() - suffix.size()), suffix, suffix.size()) == 0;
    }

    /// Return true if the given string is a substring of *this.
    [[nodiscard]] constexpr bool contains(StringRef other) const {
        return find(other) != npos;
    }

    /// Return true if the given character is contained in *this.
    [[nodiscard]] constexpr bool contains(char c) const {
        return find(c) != npos;
    }

    /// Return true if the given string is a substring of *this, ignoring case.
    [[nodiscard]] constexpr bool contains_insensitive(StringRef other) const {
        return find_insensitive(other) != npos;
    }

    // --- Searching ---

    /// Search for the first character satisfying the predicate.
    template <typename F>
    [[nodiscard]] constexpr usize find_if(F f, usize from = 0) const {
        from = std::min(from, size());
        StringRef s = drop_front(from);
        while (!s.empty()) {
            if (f(s.front())) {
                return size() - s.size();
            }
            s = s.drop_front();
        }
        return npos;
    }

    /// Search for the first character not satisfying the predicate.
    template <typename F>
    [[nodiscard]] constexpr usize find_if_not(F f, usize from = 0) const {
        return find_if([&](char c) { return !f(c); }, from);
    }

    /// Search for the first character, ignoring case.
    [[nodiscard]] constexpr usize find_insensitive(char c, usize from = 0) const {
        char l = to_lower(c);
        return find_if([l](char d) { return to_lower(d) == l; }, from);
    }

    /// Search for the first occurrence of a string, ignoring case.
    [[nodiscard]] constexpr usize find_insensitive(StringRef str,
                                                   usize from = 0) const {
        from = std::min(from, size());
        StringRef s = drop_front(from);
        while (s.size() >= str.size()) {
            if (s.starts_with_insensitive(str)) {
                return from;
            }
            s = s.drop_front();
            ++from;
        }
        return npos;
    }

    /// Search for the last character, ignoring case.
    [[nodiscard]] constexpr usize rfind_insensitive(char c, usize from = npos) const {
        if (empty()) {
            return npos;
        }
        from = std::min(from, size() - 1);
        for (usize i = from + 1; i != 0;) {
            --i;
            if (to_lower(data()[i]) == to_lower(c)) {
                return i;
            }
        }
        return npos;
    }

    /// Search for the last occurrence of a string, ignoring case.
    [[nodiscard]] constexpr usize rfind_insensitive(StringRef str) const {
        usize n = str.size();
        if (n > size()) {
            return npos;
        }
        for (usize i = size() - n + 1; i != 0;) {
            --i;
            if (substr(i, n).equals_insensitive(str)) {
                return i;
            }
        }
        return npos;
    }

    /// Find the last character in the string that is not c, or npos if not found.
    [[nodiscard]] constexpr usize find_last_not_of(char c, usize from = npos) const {
        if (empty()) {
            return npos;
        }
        from = std::min(from, size() - 1);
        for (usize i = from + 1; i != 0;) {
            --i;
            if (data()[i] != c) {
                return i;
            }
        }
        return npos;
    }

    // --- Count ---

    /// Return the number of occurrences of c in the string.
    [[nodiscard]] constexpr usize count(char c) const {
        usize result = 0;
        for (usize i = 0; i < size(); ++i) {
            if (data()[i] == c) {
                ++result;
            }
        }
        return result;
    }

    /// Return the number of non-overlapped occurrences of str in the string.
    [[nodiscard]] constexpr usize count(StringRef str) const {
        usize result = 0;
        usize n = str.size();
        if (n == 0) {
            return 0;
        }
        usize pos = 0;
        while ((pos = find(str, pos)) != npos) {
            ++result;
            pos += n;
        }
        return result;
    }

    // --- Substring / Slice ---

    /// Return a reference to the substring from [start, start + n).
    [[nodiscard]] constexpr StringRef substr(usize start, usize n = npos) const {
        start = std::min(start, size());
        return StringRef(data() + start, std::min(n, size() - start));
    }

    /// Return a reference to the substring from [start, end).
    [[nodiscard]] constexpr StringRef slice(usize start, usize end) const {
        start = std::min(start, size());
        end = std::clamp(end, start, size());
        return StringRef(data() + start, end - start);
    }

    // --- Take / Drop ---

    /// Return a StringRef with only the first n elements remaining.
    [[nodiscard]] constexpr StringRef take_front(usize n = 1) const {
        if (n >= size()) {
            return *this;
        }
        return drop_back(size() - n);
    }

    /// Return a StringRef with only the last n elements remaining.
    [[nodiscard]] constexpr StringRef take_back(usize n = 1) const {
        if (n >= size()) {
            return *this;
        }
        return drop_front(size() - n);
    }

    /// Return the longest prefix where every character satisfies the predicate.
    template <typename F>
    [[nodiscard]] constexpr StringRef take_while(F f) const {
        return substr(0, find_if_not(f));
    }

    /// Return the longest prefix where no character satisfies the predicate.
    template <typename F>
    [[nodiscard]] constexpr StringRef take_until(F f) const {
        return substr(0, find_if(f));
    }

    /// Return a StringRef with the first n elements dropped.
    [[nodiscard]] constexpr StringRef drop_front(usize n = 1) const {
        assert(size() >= n && "Dropping more elements than exist");
        return substr(n);
    }

    /// Return a StringRef with the last n elements dropped.
    [[nodiscard]] constexpr StringRef drop_back(usize n = 1) const {
        assert(size() >= n && "Dropping more elements than exist");
        return substr(0, size() - n);
    }

    /// Drop characters from the front while they satisfy the predicate.
    template <typename F>
    [[nodiscard]] constexpr StringRef drop_while(F f) const {
        return substr(find_if_not(f));
    }

    /// Drop characters from the front until one satisfies the predicate.
    template <typename F>
    [[nodiscard]] constexpr StringRef drop_until(F f) const {
        return substr(find_if(f));
    }

    // --- Consume ---

    /// If this string starts with prefix, remove it and return true.
    constexpr bool consume_front(StringRef prefix) {
        if (!starts_with(prefix)) {
            return false;
        }
        *this = drop_front(prefix.size());
        return true;
    }

    /// If this string starts with prefix (ignoring case), remove it and return true.
    constexpr bool consume_front_insensitive(StringRef prefix) {
        if (!starts_with_insensitive(prefix)) {
            return false;
        }
        *this = drop_front(prefix.size());
        return true;
    }

    /// If this string ends with suffix, remove it and return true.
    constexpr bool consume_back(StringRef suffix) {
        if (!ends_with(suffix)) {
            return false;
        }
        *this = drop_back(suffix.size());
        return true;
    }

    /// If this string ends with suffix (ignoring case), remove it and return true.
    constexpr bool consume_back_insensitive(StringRef suffix) {
        if (!ends_with_insensitive(suffix)) {
            return false;
        }
        *this = drop_back(suffix.size());
        return true;
    }

    // --- Trim ---

    /// Return string with consecutive char characters starting from the left removed.
    [[nodiscard]] constexpr StringRef ltrim(char c) const {
        return drop_front(std::min(size(), base::find_first_not_of(c)));
    }

    /// Return string with consecutive characters in chars starting from the left removed.
    [[nodiscard]] constexpr StringRef ltrim(StringRef chars = " \t\n\v\f\r") const {
        return drop_front(std::min(size(), base::find_first_not_of(chars)));
    }

    /// Return string with consecutive char characters starting from the right removed.
    [[nodiscard]] constexpr StringRef rtrim(char c) const {
        return drop_back(size() - std::min(size(), find_last_not_of(c) + 1));
    }

    /// Return string with consecutive characters in chars starting from the right removed.
    [[nodiscard]] constexpr StringRef rtrim(StringRef chars = " \t\n\v\f\r") const {
        return drop_back(size() - std::min(size(), base::find_last_not_of(chars) + 1));
    }

    /// Return string with consecutive char characters from both sides removed.
    [[nodiscard]] constexpr StringRef trim(char c) const {
        return ltrim(c).rtrim(c);
    }

    /// Return string with consecutive characters in chars from both sides removed.
    [[nodiscard]] constexpr StringRef trim(StringRef chars = " \t\n\v\f\r") const {
        return ltrim(chars).rtrim(chars);
    }

    // --- Split ---

    /// Split into two substrings around the first occurrence of a separator character.
    [[nodiscard]] constexpr std::pair<StringRef, StringRef> split(char separator) const {
        usize idx = find(separator);
        if (idx == npos) {
            return {*this, StringRef()};
        }
        return {slice(0, idx), substr(idx + 1)};
    }

    /// Split into two substrings around the first occurrence of a separator string.
    [[nodiscard]] constexpr std::pair<StringRef, StringRef> split(StringRef separator) const {
        usize idx = find(separator);
        if (idx == npos) {
            return {*this, StringRef()};
        }
        return {slice(0, idx), substr(idx + separator.size())};
    }

    /// Split into two substrings around the last occurrence of a separator character.
    [[nodiscard]] constexpr std::pair<StringRef, StringRef> rsplit(char separator) const {
        usize idx = rfind(separator);
        if (idx == npos) {
            return {*this, StringRef()};
        }
        return {slice(0, idx), substr(idx + 1)};
    }

    /// Split into two substrings around the last occurrence of a separator string.
    [[nodiscard]] constexpr std::pair<StringRef, StringRef> rsplit(StringRef separator) const {
        usize idx = rfind(separator);
        if (idx == npos) {
            return {*this, StringRef()};
        }
        return {slice(0, idx), substr(idx + separator.size())};
    }

    /// Split into substrings around occurrences of a separator.
    /// Calls callback(StringRef) for each part.
    template <typename Callback>
    constexpr void
    split(Callback callback, char separator, i32 max_split = -1, bool keep_empty = true) const {
        StringRef s = *this;
        while (max_split-- != 0) {
            usize idx = s.find(separator);
            if (idx == npos) {
                break;
            }
            if (keep_empty || idx > 0) {
                callback(s.slice(0, idx));
            }
            s = s.substr(idx + 1);
        }
        if (keep_empty || !s.empty()) {
            callback(s);
        }
    }

    /// Split into substrings around occurrences of a separator string.
    /// Calls callback(StringRef) for each part.
    /// If separator is empty, calls callback once with the entire string.
    template <typename Callback>
    constexpr void split(Callback callback,
                         StringRef separator,
                         i32 max_split = -1,
                         bool keep_empty = true) const {
        if (separator.empty()) {
            callback(*this);
            return;
        }
        StringRef s = *this;
        while (max_split-- != 0) {
            usize idx = s.find(separator);
            if (idx == npos) {
                break;
            }
            if (keep_empty || idx > 0) {
                callback(s.slice(0, idx));
            }
            s = s.substr(idx + separator.size());
        }
        if (keep_empty || !s.empty()) {
            callback(s);
        }
    }

    // --- Case Conversion ---

    /// Convert the string to lowercase (returns std::string).
    [[nodiscard]] constexpr std::string lower() const {
        std::string result;
        result.reserve(size());
        for (char c : *this) {
            result.push_back(to_lower(c));
        }
        return result;
    }

    /// Convert the string to uppercase (returns std::string).
    [[nodiscard]] constexpr std::string upper() const {
        std::string result;
        result.reserve(size());
        for (char c : *this) {
            result.push_back(to_upper(c));
        }
        return result;
    }

    // --- Conversion ---

    /// Get the contents as an std::string.
    [[nodiscard]] constexpr std::string str() const {
        return std::string(data(), size());
    }
};

} // namespace lighter
