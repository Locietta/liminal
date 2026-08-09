#pragma once

#include <algorithm>
#include <compare>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/utils/memory.h>

namespace lighter {

/// A map backed by parallel, dynamically allocated key and value arrays.
///
/// Binary search touches only the densely packed keys. Growth relocates each
/// array independently through Liminal's trivially-relocatable-aware memory
/// primitives. Lookup is logarithmic; insertion and erasure are linear.
template <typename Key, typename T, typename Compare = std::less<>>
struct FlatMap {
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<key_type, mapped_type>;
    using key_compare = Compare;
    using size_type = usize;
    using difference_type = isize;
    using reference = std::pair<const key_type &, mapped_type &>;
    using const_reference = std::pair<const key_type &, const mapped_type &>;

    template <bool Const>
    struct BasicIterator {
        using iterator_concept = std::random_access_iterator_tag;
        using iterator_category = std::random_access_iterator_tag;
        using value_type = FlatMap::value_type;
        using difference_type = FlatMap::difference_type;
        using reference = std::conditional_t<Const, FlatMap::const_reference, FlatMap::reference>;

        struct ArrowProxy {
            reference value;

            [[nodiscard]] constexpr const reference *operator->() const noexcept { return std::addressof(value); }
        };

        constexpr BasicIterator() = default;

        template <bool OtherConst>
        constexpr BasicIterator(const BasicIterator<OtherConst> &other) noexcept
            requires(Const && !OtherConst)
            : key_(other.key_), mapped_(other.mapped_) {}

        [[nodiscard]] constexpr reference operator*() const noexcept { return {*key_, *mapped_}; }
        [[nodiscard]] constexpr ArrowProxy operator->() const noexcept { return {**this}; }
        [[nodiscard]] constexpr reference operator[](difference_type offset) const noexcept { return *(*this + offset); }

        constexpr BasicIterator &operator++() noexcept {
            ++key_;
            ++mapped_;
            return *this;
        }

        constexpr BasicIterator operator++(int) noexcept {
            BasicIterator result = *this;
            ++*this;
            return result;
        }

        constexpr BasicIterator &operator--() noexcept {
            --key_;
            --mapped_;
            return *this;
        }

        constexpr BasicIterator operator--(int) noexcept {
            BasicIterator result = *this;
            --*this;
            return result;
        }

        constexpr BasicIterator &operator+=(difference_type offset) noexcept {
            if (offset == 0) return *this;
            key_ += offset;
            mapped_ += offset;
            return *this;
        }

        constexpr BasicIterator &operator-=(difference_type offset) noexcept { return *this += -offset; }

        friend constexpr BasicIterator operator+(BasicIterator iterator, difference_type offset) noexcept { return iterator += offset; }
        friend constexpr BasicIterator operator+(difference_type offset, BasicIterator iterator) noexcept { return iterator += offset; }
        friend constexpr BasicIterator operator-(BasicIterator iterator, difference_type offset) noexcept { return iterator -= offset; }

        template <bool OtherConst>
        friend constexpr difference_type operator-(const BasicIterator &left, const BasicIterator<OtherConst> &right) noexcept {
            return left.key_ == nullptr ? 0 : left.key_ - right.key_;
        }

        template <bool OtherConst>
        friend constexpr bool operator==(const BasicIterator &left, const BasicIterator<OtherConst> &right) noexcept {
            return left.key_ == right.key_;
        }

        template <bool OtherConst>
        friend constexpr auto operator<=>(const BasicIterator &left, const BasicIterator<OtherConst> &right) noexcept {
            return std::compare_three_way{}(left.key_, right.key_);
        }

    private:
        using mapped_pointer = std::conditional_t<Const, const mapped_type *, mapped_type *>;

        constexpr BasicIterator(const key_type *key, mapped_pointer mapped) noexcept : key_(key), mapped_(mapped) {}

        template <bool>
        friend struct BasicIterator;
        friend FlatMap;

        const key_type *key_ = nullptr;
        mapped_pointer mapped_ = nullptr;
    };

    using iterator = BasicIterator<false>;
    using const_iterator = BasicIterator<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    constexpr FlatMap() = default;

    constexpr explicit FlatMap(const key_compare &compare) : compare_(compare) {}

    constexpr FlatMap(std::initializer_list<value_type> values, const key_compare &compare = {}) : compare_(compare) {
        construct_from_unsorted_range(values);
    }

    template <std::ranges::input_range Range>
        requires(!std::same_as<std::remove_cvref_t<Range>, FlatMap> &&
                 std::constructible_from<value_type, std::ranges::range_reference_t<Range>>)
    constexpr explicit FlatMap(Range &&values, const key_compare &compare = {}) : compare_(compare) {
        construct_from_unsorted_range(std::forward<Range>(values));
    }

    constexpr FlatMap(const FlatMap &other)
        requires std::copy_constructible<key_type> && std::copy_constructible<mapped_type> && std::copy_constructible<key_compare>
        : compare_(other.compare_) {
        if (other.empty()) return;

        mem::AllocationGuard<key_type> key_guard(other.size_);
        mem::AllocationGuard<mapped_type> mapped_guard(other.size_);
        key_guard.mark(mem::uninitialized_copy<key_type>(other.keys(), key_guard.data()));
        mapped_guard.mark(mem::uninitialized_copy<mapped_type>(other.mapped_values(), mapped_guard.data()));
        keys_ = key_guard.release();
        mapped_ = mapped_guard.release();
        size_ = other.size_;
        capacity_ = other.size_;
    }

    constexpr FlatMap(FlatMap &&other) noexcept(std::is_nothrow_move_constructible_v<key_compare>)
        : compare_(std::move(other.compare_)), keys_(std::exchange(other.keys_, nullptr)), mapped_(std::exchange(other.mapped_, nullptr)),
          size_(std::exchange(other.size_, 0)), capacity_(std::exchange(other.capacity_, 0)) {}

    constexpr FlatMap &operator=(const FlatMap &other)
        requires std::copy_constructible<key_type> && std::copy_constructible<mapped_type> && std::copy_constructible<key_compare> &&
                 std::swappable<key_compare>
    {
        if (this == &other) return *this;
        FlatMap replacement(other);
        swap(replacement);
        return *this;
    }

    constexpr FlatMap &operator=(FlatMap &&other) noexcept(std::is_nothrow_move_constructible_v<key_compare> &&
                                                           std::is_nothrow_swappable_v<key_compare>)
        requires std::movable<key_compare>
    {
        if (this == &other) return *this;
        FlatMap replacement(std::move(other));
        swap(replacement);
        return *this;
    }

    constexpr ~FlatMap() { destroy_and_deallocate(); }

    [[nodiscard]] constexpr iterator begin() noexcept { return {keys_, mapped_}; }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return {keys_, mapped_}; }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] constexpr iterator end() noexcept { return {offset(keys_, size_), offset(mapped_, size_)}; }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return {offset(keys_, size_), offset(mapped_, size_)}; }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return end(); }

    [[nodiscard]] constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    [[nodiscard]] constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(begin()); }

    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
    [[nodiscard]] constexpr size_type capacity() const noexcept { return capacity_; }
    [[nodiscard]] constexpr size_type max_size() const noexcept {
        constexpr size_type by_key_size = (std::numeric_limits<size_type>::max)() / sizeof(key_type);
        constexpr size_type by_mapped_size = (std::numeric_limits<size_type>::max)() / sizeof(mapped_type);
        constexpr size_type by_difference = static_cast<size_type>((std::numeric_limits<difference_type>::max)());
        return (std::min) ({by_key_size, by_mapped_size, by_difference});
    }

    constexpr void reserve(size_type requested_capacity) {
        if (requested_capacity <= capacity_) return;
        replace_allocation(requested_capacity);
    }

    constexpr void shrink_to_fit() {
        if (size_ == capacity_) return;
        if (empty()) {
            destroy_and_deallocate();
            return;
        }
        replace_allocation(size_);
    }

    [[nodiscard]] constexpr const key_type *keys_data() const noexcept { return keys_; }
    [[nodiscard]] constexpr mapped_type *values_data() noexcept { return mapped_; }
    [[nodiscard]] constexpr const mapped_type *values_data() const noexcept { return mapped_; }
    [[nodiscard]] constexpr key_compare key_comp() const { return compare_; }

    template <typename LookupKey>
    [[nodiscard]] constexpr iterator lower_bound(const LookupKey &key) {
        const key_type *position = std::ranges::lower_bound(keys(), key, compare_);
        const size_type index = index_of_key(position);
        return {position, offset(mapped_, index)};
    }

    template <typename LookupKey>
    [[nodiscard]] constexpr const_iterator lower_bound(const LookupKey &key) const {
        const key_type *position = std::ranges::lower_bound(keys(), key, compare_);
        const size_type index = index_of_key(position);
        return {position, offset(mapped_, index)};
    }

    template <typename LookupKey>
    [[nodiscard]] constexpr iterator find(const LookupKey &key) {
        const iterator candidate = lower_bound(key);
        return candidate != end() && equivalent(candidate->first, key) ? candidate : end();
    }

    template <typename LookupKey>
    [[nodiscard]] constexpr const_iterator find(const LookupKey &key) const {
        const const_iterator candidate = lower_bound(key);
        return candidate != end() && equivalent(candidate->first, key) ? candidate : end();
    }

    template <typename LookupKey>
    [[nodiscard]] constexpr bool contains(const LookupKey &key) const {
        return find(key) != end();
    }

    template <typename LookupKey>
    [[nodiscard]] constexpr size_type count(const LookupKey &key) const {
        return contains(key) ? 1 : 0;
    }

    [[nodiscard]] constexpr mapped_type &at(const key_type &key) {
        const iterator found = find(key);
        if (found == end()) LIGHTER_THROW(std::out_of_range("FlatMap key not found"));
        return found->second;
    }

    [[nodiscard]] constexpr const mapped_type &at(const key_type &key) const {
        const const_iterator found = find(key);
        if (found == end()) LIGHTER_THROW(std::out_of_range("FlatMap key not found"));
        return found->second;
    }

    [[nodiscard]] constexpr mapped_type &operator[](const key_type &key) { return try_emplace(key).first->second; }
    [[nodiscard]] constexpr mapped_type &operator[](key_type &&key) { return try_emplace(std::move(key)).first->second; }

    constexpr std::pair<iterator, bool> insert(const value_type &value) { return insert_value(value); }
    constexpr std::pair<iterator, bool> insert(value_type &&value) { return insert_value(std::move(value)); }

    constexpr void insert(std::initializer_list<value_type> values) {
        checked_size(values.size());
        reserve(size_ + values.size());
        for (const value_type &value : values) insert(value);
    }

    template <typename... Args>
    constexpr std::pair<iterator, bool> try_emplace(const key_type &key, Args &&...args) {
        return try_emplace_key(key, std::forward<Args>(args)...);
    }

    template <typename... Args>
    constexpr std::pair<iterator, bool> try_emplace(key_type &&key, Args &&...args) {
        return try_emplace_key(std::move(key), std::forward<Args>(args)...);
    }

    template <typename Mapped>
    constexpr std::pair<iterator, bool> insert_or_assign(const key_type &key, Mapped &&value) {
        return insert_or_assign_key(key, std::forward<Mapped>(value));
    }

    template <typename Mapped>
    constexpr std::pair<iterator, bool> insert_or_assign(key_type &&key, Mapped &&value) {
        return insert_or_assign_key(std::move(key), std::forward<Mapped>(value));
    }

    constexpr iterator erase(const_iterator position) { return erase(position, position + 1); }

    constexpr iterator erase(const_iterator first, const_iterator last) {
        const size_type first_index = index_of(first);
        if (first == last) return iterator_at(first_index);
        const size_type count = static_cast<size_type>(last - first);

        if constexpr (std::is_nothrow_move_assignable_v<key_type> && std::is_nothrow_move_assignable_v<mapped_type>) {
            std::ranges::move(offset(keys_, first_index + count), offset(keys_, size_), offset(keys_, first_index));
            std::ranges::move(offset(mapped_, first_index + count), offset(mapped_, size_), offset(mapped_, first_index));
            mem::destroy_range(range_from(offset(keys_, size_ - count), count));
            mem::destroy_range(range_from(offset(mapped_, size_ - count), count));
            size_ -= count;
        } else {
            erase_reallocate(first_index, count);
        }
        return iterator_at(first_index);
    }

    constexpr size_type erase(const key_type &key) {
        const iterator found = find(key);
        if (found == end()) return 0;
        erase(found);
        return 1;
    }

    constexpr void clear() noexcept {
        mem::destroy_range(keys());
        mem::destroy_range(mapped_values());
        size_ = 0;
    }

    constexpr void swap(FlatMap &other) noexcept(std::is_nothrow_swappable_v<key_compare>) {
        using std::swap;
        swap(compare_, other.compare_);
        swap(keys_, other.keys_);
        swap(mapped_, other.mapped_);
        swap(size_, other.size_);
        swap(capacity_, other.capacity_);
    }

    friend constexpr bool operator==(const FlatMap &left, const FlatMap &right) {
        return std::ranges::equal(left.keys(), right.keys()) && std::ranges::equal(left.mapped_values(), right.mapped_values());
    }

private:
    template <typename U>
    static constexpr bool k_relocate_instead_of_copy =
        mem::is_trivially_relocatable_v<U> || std::is_nothrow_move_constructible_v<U> || !std::copy_constructible<U>;

    template <typename U>
    static constexpr bool k_relocation_is_nothrow = mem::is_trivially_relocatable_v<U> || std::is_nothrow_move_constructible_v<U>;

    template <typename U>
    struct SplitConstruction {
        U *prefix_end;
        bool middle_constructed = false;
        U *suffix_begin;
        U *suffix_end;
    };

    template <std::ranges::input_range Range>
    constexpr void construct_from_unsorted_range(Range &&values) {
        std::vector<value_type> sorted;
        if constexpr (std::ranges::sized_range<Range>) sorted.reserve(std::ranges::size(values));
        for (auto &&value : values) sorted.emplace_back(std::forward<decltype(value)>(value));

        std::stable_sort(sorted.begin(), sorted.end(), [this](const value_type &left, const value_type &right) {
            return std::invoke(compare_, left.first, right.first);
        });
        const auto duplicate_begin = std::ranges::unique(sorted, [this](const value_type &left, const value_type &right) {
                                         return equivalent(left.first, right.first);
                                     }).begin();
        sorted.erase(duplicate_begin, sorted.end());
        if (sorted.empty()) return;
        if (sorted.size() > max_size()) LIGHTER_THROW(std::length_error("FlatMap capacity overflow"));

        mem::AllocationGuard<key_type> key_guard(sorted.size());
        mem::AllocationGuard<mapped_type> mapped_guard(sorted.size());
        key_type *key_out = key_guard.data();
        mapped_type *mapped_out = mapped_guard.data();
        for (value_type &value : sorted) {
            mem::construct(key_out, std::move_if_noexcept(value.first));
            key_guard.mark(++key_out);
        }
        for (value_type &value : sorted) {
            mem::construct(mapped_out, std::move_if_noexcept(value.second));
            mapped_guard.mark(++mapped_out);
        }

        keys_ = key_guard.release();
        mapped_ = mapped_guard.release();
        size_ = sorted.size();
        capacity_ = sorted.size();
    }

    [[nodiscard]] constexpr auto keys() noexcept { return range_from(keys_, size_); }
    [[nodiscard]] constexpr auto keys() const noexcept { return range_from(keys_, size_); }
    [[nodiscard]] constexpr auto mapped_values() noexcept { return range_from(mapped_, size_); }
    [[nodiscard]] constexpr auto mapped_values() const noexcept { return range_from(mapped_, size_); }

    [[nodiscard]] constexpr size_type checked_size(size_type extra) const {
        if (extra > max_size() - size_) LIGHTER_THROW(std::length_error("FlatMap capacity overflow"));
        return size_ + extra;
    }

    [[nodiscard]] constexpr size_type next_capacity() const {
        const size_type minimum = checked_size(1);
        size_type grown = capacity_ == 0 ? 1 : capacity_ * 2;
        if (grown < capacity_ || grown < minimum) grown = minimum;
        if (grown > max_size()) grown = max_size();
        return grown;
    }

    template <typename Left, typename Right>
    [[nodiscard]] constexpr bool equivalent(const Left &left, const Right &right) const {
        return !std::invoke(compare_, left, right) && !std::invoke(compare_, right, left);
    }

    constexpr void destroy_and_deallocate() noexcept {
        mem::destroy_range(keys());
        mem::destroy_range(mapped_values());
        mem::deallocate(keys_, capacity_);
        mem::deallocate(mapped_, capacity_);
        keys_ = nullptr;
        mapped_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

    template <typename U>
    constexpr static U *transfer_range(std::ranges::forward_range auto &&source, U *destination) {
        if constexpr (k_relocate_instead_of_copy<U>) {
            return mem::uninitialized_relocate(std::forward<decltype(source)>(source), destination);
        } else {
            return mem::uninitialized_copy<U>(std::forward<decltype(source)>(source), destination);
        }
    }

    template <typename U>
    constexpr static void destroy_transferred_source(std::ranges::forward_range auto &&source) noexcept {
        if constexpr (k_relocate_instead_of_copy<U>) {
            mem::destroy_relocated_source(std::forward<decltype(source)>(source));
        } else {
            mem::destroy_range(std::forward<decltype(source)>(source));
        }
    }

    constexpr void replace_allocation(size_type new_capacity) {
        if (new_capacity > max_size()) LIGHTER_THROW(std::length_error("FlatMap capacity overflow"));

        mem::AllocationGuard<key_type> key_guard(new_capacity);
        mem::AllocationGuard<mapped_type> mapped_guard(new_capacity);

        if constexpr (!k_relocate_instead_of_copy<key_type>) key_guard.mark(transfer_range<key_type>(keys(), key_guard.data()));
        if constexpr (!k_relocate_instead_of_copy<mapped_type>)
            mapped_guard.mark(transfer_range<mapped_type>(mapped_values(), mapped_guard.data()));
        if constexpr (k_relocate_instead_of_copy<key_type> && !k_relocation_is_nothrow<key_type>)
            key_guard.mark(transfer_range<key_type>(keys(), key_guard.data()));
        if constexpr (k_relocate_instead_of_copy<mapped_type> && !k_relocation_is_nothrow<mapped_type>)
            mapped_guard.mark(transfer_range<mapped_type>(mapped_values(), mapped_guard.data()));
        if constexpr (k_relocate_instead_of_copy<key_type> && k_relocation_is_nothrow<key_type>)
            key_guard.mark(transfer_range<key_type>(keys(), key_guard.data()));
        if constexpr (k_relocate_instead_of_copy<mapped_type> && k_relocation_is_nothrow<mapped_type>)
            mapped_guard.mark(transfer_range<mapped_type>(mapped_values(), mapped_guard.data()));

        key_type *old_keys = keys_;
        mapped_type *old_mapped = mapped_;
        const size_type old_capacity = capacity_;
        keys_ = key_guard.release();
        mapped_ = mapped_guard.release();
        capacity_ = new_capacity;

        destroy_transferred_source<key_type>(range_from(old_keys, size_));
        destroy_transferred_source<mapped_type>(range_from(old_mapped, size_));
        mem::deallocate(old_keys, old_capacity);
        mem::deallocate(old_mapped, old_capacity);
    }

    template <typename KeyTemporary, typename MappedTemporary>
    constexpr iterator insert_constructed(size_type index, KeyTemporary &key, MappedTemporary &mapped) {
        if (size_ == capacity_) return insert_reallocate(index, key, mapped, next_capacity());

        if (index == size_) {
            mem::construct(offset(keys_, size_), key.release());
            LIGHTER_TRY { mem::construct(offset(mapped_, size_), mapped.release()); }
            LIGHTER_CATCH_ALL() {
                mem::destroy(offset(keys_, size_));
                LIGHTER_RETHROW();
            }
            ++size_;
            return end() - 1;
        }

        constexpr bool k_nothrow_in_place = std::is_nothrow_move_constructible_v<key_type> && std::is_nothrow_move_assignable_v<key_type> &&
                                            std::is_nothrow_move_constructible_v<mapped_type> &&
                                            std::is_nothrow_move_assignable_v<mapped_type>;
        if constexpr (!k_nothrow_in_place) {
            return insert_reallocate(index, key, mapped, capacity_);
        } else {
            key_type *key_position = offset(keys_, index);
            mapped_type *mapped_position = offset(mapped_, index);
            key_type *old_key_end = offset(keys_, size_);
            mapped_type *old_mapped_end = offset(mapped_, size_);
            mem::construct(old_key_end, std::move(*(old_key_end - 1)));
            mem::construct(old_mapped_end, std::move(*(old_mapped_end - 1)));
            std::ranges::move_backward(key_position, old_key_end - 1, old_key_end);
            std::ranges::move_backward(mapped_position, old_mapped_end - 1, old_mapped_end);
            *key_position = key.release();
            *mapped_position = mapped.release();
            ++size_;
            return iterator_at(index);
        }
    }

    template <typename U>
    constexpr static void transfer_around_gap(U *old_data, size_type old_size, U *new_data, size_type index, SplitConstruction<U> &state) {
        state.prefix_end = transfer_range<U>(range_from(old_data, index), new_data);
        state.suffix_end = transfer_range<U>(range_from(offset(old_data, index), old_size - index), state.suffix_begin);
    }

    template <typename U>
    constexpr static void destroy_split(U *begin, SplitConstruction<U> &state) noexcept {
        mem::destroy_range(std::ranges::subrange(begin, state.prefix_end));
        if (state.middle_constructed) mem::destroy(state.suffix_begin - 1);
        mem::destroy_range(std::ranges::subrange(state.suffix_begin, state.suffix_end));
    }

    template <typename KeyTemporary, typename MappedTemporary>
    constexpr iterator insert_reallocate(size_type index, KeyTemporary &key, MappedTemporary &mapped, size_type new_capacity) {
        key_type *new_keys = mem::allocate<key_type>(new_capacity);
        mapped_type *new_mapped = nullptr;
        LIGHTER_TRY { new_mapped = mem::allocate<mapped_type>(new_capacity); }
        LIGHTER_CATCH_ALL() {
            mem::deallocate(new_keys, new_capacity);
            LIGHTER_RETHROW();
        }

        SplitConstruction<key_type> key_state{
            .prefix_end = new_keys, .suffix_begin = offset(new_keys, index + 1), .suffix_end = offset(new_keys, index + 1)};
        SplitConstruction<mapped_type> mapped_state{
            .prefix_end = new_mapped, .suffix_begin = offset(new_mapped, index + 1), .suffix_end = offset(new_mapped, index + 1)};

        LIGHTER_TRY {
            mem::construct(offset(new_keys, index), key.release());
            key_state.middle_constructed = true;
            mem::construct(offset(new_mapped, index), mapped.release());
            mapped_state.middle_constructed = true;

            if constexpr (!k_relocate_instead_of_copy<key_type>) transfer_around_gap(keys_, size_, new_keys, index, key_state);
            if constexpr (!k_relocate_instead_of_copy<mapped_type>) transfer_around_gap(mapped_, size_, new_mapped, index, mapped_state);
            if constexpr (k_relocate_instead_of_copy<key_type> && !k_relocation_is_nothrow<key_type>)
                transfer_around_gap(keys_, size_, new_keys, index, key_state);
            if constexpr (k_relocate_instead_of_copy<mapped_type> && !k_relocation_is_nothrow<mapped_type>)
                transfer_around_gap(mapped_, size_, new_mapped, index, mapped_state);
            if constexpr (k_relocate_instead_of_copy<key_type> && k_relocation_is_nothrow<key_type>)
                transfer_around_gap(keys_, size_, new_keys, index, key_state);
            if constexpr (k_relocate_instead_of_copy<mapped_type> && k_relocation_is_nothrow<mapped_type>)
                transfer_around_gap(mapped_, size_, new_mapped, index, mapped_state);
        }
        LIGHTER_CATCH_ALL() {
            destroy_split(new_keys, key_state);
            destroy_split(new_mapped, mapped_state);
            mem::deallocate(new_keys, new_capacity);
            mem::deallocate(new_mapped, new_capacity);
            LIGHTER_RETHROW();
        }

        key_type *old_keys = keys_;
        mapped_type *old_mapped = mapped_;
        const size_type old_size = size_;
        const size_type old_capacity = capacity_;
        keys_ = new_keys;
        mapped_ = new_mapped;
        size_ = old_size + 1;
        capacity_ = new_capacity;

        destroy_transferred_source<key_type>(range_from(old_keys, old_size));
        destroy_transferred_source<mapped_type>(range_from(old_mapped, old_size));
        mem::deallocate(old_keys, old_capacity);
        mem::deallocate(old_mapped, old_capacity);
        return iterator_at(index);
    }

    template <typename Value>
    constexpr std::pair<iterator, bool> insert_value(Value &&value) {
        const iterator position = lower_bound(value.first);
        if (position != end() && equivalent(position->first, value.first)) return {position, false};

        const size_type index = index_of(position);
        mem::StackTemporary<key_type> key(std::in_place, std::forward<Value>(value).first);
        mem::StackTemporary<mapped_type> mapped(std::in_place, std::forward<Value>(value).second);
        return {insert_constructed(index, key, mapped), true};
    }

    template <typename KeyArgument, typename... Args>
    constexpr std::pair<iterator, bool> try_emplace_key(KeyArgument &&key_argument, Args &&...args) {
        const iterator position = lower_bound(key_argument);
        if (position != end() && equivalent(position->first, key_argument)) return {position, false};

        const size_type index = index_of(position);
        mem::StackTemporary<key_type> key(std::in_place, std::forward<KeyArgument>(key_argument));
        mem::StackTemporary<mapped_type> mapped(std::in_place, std::forward<Args>(args)...);
        return {insert_constructed(index, key, mapped), true};
    }

    template <typename KeyArgument, typename Mapped>
    constexpr std::pair<iterator, bool> insert_or_assign_key(KeyArgument &&key, Mapped &&value) {
        const iterator position = lower_bound(key);
        if (position != end() && equivalent(position->first, key)) {
            position->second = std::forward<Mapped>(value);
            return {position, false};
        }
        return try_emplace_key(std::forward<KeyArgument>(key), std::forward<Mapped>(value));
    }

    template <typename U>
    constexpr static U *transfer_without_range(U *old_data, size_type old_size, U *new_data, size_type first, size_type count) {
        U *out = transfer_range<U>(range_from(old_data, first), new_data);
        LIGHTER_TRY { return transfer_range<U>(range_from(offset(old_data, first + count), old_size - first - count), out); }
        LIGHTER_CATCH_ALL() {
            mem::destroy_range(std::ranges::subrange(new_data, out));
            LIGHTER_RETHROW();
        }
    }

    constexpr void erase_reallocate(size_type first, size_type count) {
        if (count == size_) {
            clear();
            return;
        }

        mem::AllocationGuard<key_type> key_guard(capacity_);
        mem::AllocationGuard<mapped_type> mapped_guard(capacity_);

        if constexpr (!k_relocate_instead_of_copy<key_type>)
            key_guard.mark(transfer_without_range(keys_, size_, key_guard.data(), first, count));
        if constexpr (!k_relocate_instead_of_copy<mapped_type>)
            mapped_guard.mark(transfer_without_range(mapped_, size_, mapped_guard.data(), first, count));
        if constexpr (k_relocate_instead_of_copy<key_type> && !k_relocation_is_nothrow<key_type>)
            key_guard.mark(transfer_without_range(keys_, size_, key_guard.data(), first, count));
        if constexpr (k_relocate_instead_of_copy<mapped_type> && !k_relocation_is_nothrow<mapped_type>)
            mapped_guard.mark(transfer_without_range(mapped_, size_, mapped_guard.data(), first, count));
        if constexpr (k_relocate_instead_of_copy<key_type> && k_relocation_is_nothrow<key_type>)
            key_guard.mark(transfer_without_range(keys_, size_, key_guard.data(), first, count));
        if constexpr (k_relocate_instead_of_copy<mapped_type> && k_relocation_is_nothrow<mapped_type>)
            mapped_guard.mark(transfer_without_range(mapped_, size_, mapped_guard.data(), first, count));

        key_type *old_keys = keys_;
        mapped_type *old_mapped = mapped_;
        const size_type old_size = size_;
        keys_ = key_guard.release();
        mapped_ = mapped_guard.release();
        size_ -= count;

        destroy_erased_source<key_type>(old_keys, old_size, first, count);
        destroy_erased_source<mapped_type>(old_mapped, old_size, first, count);
        mem::deallocate(old_keys, capacity_);
        mem::deallocate(old_mapped, capacity_);
    }

    template <typename U>
    constexpr static void destroy_erased_source(U *old_data, size_type old_size, size_type first, size_type count) noexcept {
        if constexpr (k_relocate_instead_of_copy<U>) {
            mem::destroy_relocated_source(range_from(old_data, first));
            mem::destroy_range(range_from(offset(old_data, first), count));
            mem::destroy_relocated_source(range_from(offset(old_data, first + count), old_size - first - count));
        } else {
            mem::destroy_range(range_from(old_data, old_size));
        }
    }

    [[nodiscard]] constexpr iterator iterator_at(size_type index) noexcept { return {offset(keys_, index), offset(mapped_, index)}; }

    [[nodiscard]] constexpr size_type index_of(const_iterator position) const noexcept { return index_of_key(position.key_); }

    [[nodiscard]] constexpr size_type index_of_key(const key_type *position) const noexcept {
        return empty() ? 0 : static_cast<size_type>(position - keys_);
    }

    template <typename U>
    [[nodiscard]] constexpr static U *offset(U *first, size_type count) noexcept {
        return count == 0 ? first : first + static_cast<difference_type>(count);
    }

    template <typename U>
    [[nodiscard]] constexpr static auto range_from(U *first, size_type count) noexcept {
        return std::ranges::subrange(first, offset(first, count));
    }

    LIGHTER_NO_UNIQUE_ADDRESS key_compare compare_{};
    key_type *keys_ = nullptr;
    mapped_type *mapped_ = nullptr;
    size_type size_ = 0;
    size_type capacity_ = 0;
};

template <typename Key, typename T, typename Compare>
constexpr void swap(FlatMap<Key, T, Compare> &left, FlatMap<Key, T, Compare> &right) noexcept(noexcept(left.swap(right))) {
    left.swap(right);
}

} // namespace lighter
