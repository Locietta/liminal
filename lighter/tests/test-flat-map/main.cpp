#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/utils/flat_map.h>
#include <lighter/utils/relocation.h>

namespace {

std::atomic<lighter::usize> live_allocations = 0;

} // namespace

void *operator new(std::size_t size) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void *allocation = std::malloc(size)) return allocation;
    live_allocations.fetch_sub(1, std::memory_order_relaxed);
    throw std::bad_alloc();
}

void operator delete(void *allocation) noexcept {
    if (allocation != nullptr) live_allocations.fetch_sub(1, std::memory_order_relaxed);
    std::free(allocation);
}

void operator delete(void *allocation, std::size_t) noexcept { ::operator delete(allocation); }

namespace {

using namespace lighter;
using namespace std::literals;

static_assert(std::random_access_iterator<FlatMap<int, int>::iterator>);
static_assert(std::random_access_iterator<FlatMap<int, int>::const_iterator>);
static_assert(std::same_as<std::iter_reference_t<FlatMap<int, int>::iterator>, std::pair<const int &, int &>>);
static_assert(std::same_as<std::iter_reference_t<FlatMap<int, int>::const_iterator>, std::pair<const int &, const int &>>);
static_assert(mem::is_trivially_relocatable_v<FlatMap<int, std::unique_ptr<int>>::value_type>);

struct AsciiCaseInsensitiveLess {
    [[nodiscard]] constexpr bool operator()(std::string_view left, std::string_view right) const noexcept {
        const usize common_size = std::min(left.size(), right.size());
        for (usize index = 0; index < common_size; ++index) {
            const char left_value = lower(left[index]);
            const char right_value = lower(right[index]);
            if (left_value != right_value) return left_value < right_value;
        }
        return left.size() < right.size();
    }

private:
    [[nodiscard]] static constexpr char lower(char value) noexcept {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    }
};

struct NoDefault {
    int value;

    constexpr explicit NoDefault(int initial) : value(initial) {}

    friend constexpr auto operator<=>(const NoDefault &, const NoDefault &) = default;
};

struct CopyPreferred {
    inline static bool fail_copy = false;

    int value;

    explicit CopyPreferred(int initial) : value(initial) {}

    CopyPreferred(const CopyPreferred &other) : value(other.value) {
        if (fail_copy) throw std::runtime_error("requested copy failure");
    }

    CopyPreferred(CopyPreferred &&other) noexcept(false) : value(other.value) { other.value = -1; }

    CopyPreferred &operator=(const CopyPreferred &) = default;
    CopyPreferred &operator=(CopyPreferred &&) noexcept(false) = default;

    friend bool operator==(const CopyPreferred &, const CopyPreferred &) = default;
};

consteval bool supports_constant_evaluation() {
    FlatMap<std::string_view, int, AsciiCaseInsensitiveLess> map;
    map.try_emplace("rust"sv, 3);
    map.try_emplace("C++"sv, 1);
    map.try_emplace("python"sv, 2);

    FlatMap<NoDefault, NoDefault> non_default;
    non_default.try_emplace(NoDefault{2}, 20);
    non_default.try_emplace(NoDefault{1}, 10);

    return map.size() == 3 && map.begin()->first == "C++"sv && map.contains("c++"sv) && map.find("PYTHON"sv)->second == 2 &&
           map.count("unknown"sv) == 0 && map.at("RUST"sv) == 3 && non_default.begin()->first.value == 1 &&
           non_default.at(NoDefault{2}).value == 20;
}

static_assert(supports_constant_evaluation());

bool modifies_and_erases() {
    FlatMap<std::string, int> map{{{"gamma", 3}, {"alpha", 1}}};

    const auto [existing, inserted_duplicate] = map.try_emplace("alpha", 9);
    if (inserted_duplicate || existing->second != 1) return false;

    const auto [assigned, inserted_assignment] = map.insert_or_assign("alpha", 7);
    if (inserted_assignment || assigned->second != 7) return false;

    map["beta"] = 2;
    if (map.size() != 3 || map.begin()->first != "alpha" || map.at("beta") != 2) return false;
    if (map.erase("alpha") != 1 || map.erase("missing") != 0 || map.contains("alpha")) return false;

    const auto after_erase = map.erase(map.begin());
    if (after_erase != map.begin() || map.size() != 1 || map.begin()->first != "gamma") return false;

    map.clear();
    return map.empty();
}

bool grows_without_a_fixed_capacity() {
    FlatMap<int, int> map;
    map.reserve(512);
    for (int value = 999; value >= 0; --value) map.try_emplace(value, value * 2);

    if (map.size() != 1000 || map.capacity() < 1000) return false;
    for (int value = 0; value < 1000; ++value) {
        const auto found = map.find(value);
        if (found == map.end() || found->second != value * 2) return false;
    }
    return true;
}

bool bulk_constructs_unsorted_ranges() {
    const std::vector<std::pair<int, std::string>> values{{4, "four"}, {1, "first"}, {3, "three"}, {1, "second"}, {2, "two"}};
    const FlatMap<int, std::string> map(values);

    if (map.size() != 4 || map.capacity() != map.size()) return false;
    if (map.at(1) != "first" || map.at(2) != "two" || map.at(3) != "three" || map.at(4) != "four") return false;
    return std::ranges::is_sorted(map, {}, [](const auto &entry) { return entry.first; });
}

bool uses_parallel_storage() {
    FlatMap<int, std::string> map{{{3, "three"}, {1, "one"}, {2, "two"}}};
    usize index = 0;
    for (auto entry : map) {
        if (std::addressof(entry.first) != map.keys_data() + static_cast<isize>(index)) return false;
        if (std::addressof(entry.second) != map.values_data() + static_cast<isize>(index)) return false;
        ++index;
    }

    map.begin()->second = "ONE";
    return map.keys_data()[0] == 1 && map.values_data()[0] == "ONE" && map.at(2) == "two";
}

bool owns_and_relocates_entries() {
    FlatMap<int, std::unique_ptr<int>> map;
    for (int value = 255; value >= 0; --value) map.try_emplace(value, std::make_unique<int>(value * 3));

    if (map.size() != 256) return false;
    for (int value = 0; value < 256; ++value) {
        const auto found = map.find(value);
        if (found == map.end() || found->second == nullptr || *found->second != value * 3) return false;
    }

    const auto old_capacity = map.capacity();
    map.reserve(old_capacity + 200);
    if (map.capacity() < old_capacity + 200) return false;
    map.shrink_to_fit();
    return map.capacity() == map.size() && *map.at(42) == 126;
}

bool copies_and_moves_storage() {
    FlatMap<std::string, int> original{{{"gamma", 3}, {"alpha", 1}, {"beta", 2}}};
    FlatMap<std::string, int> copy = original;
    copy.at("beta") = 20;
    if (original.at("beta") != 2 || copy.at("beta") != 20) return false;

    FlatMap<std::string, int> assigned;
    assigned = copy;
    if (assigned != copy) return false;

    FlatMap<std::string, int> moved = std::move(assigned);
    if (!assigned.empty() || moved.size() != 3 || moved.at("gamma") != 3) return false;

    FlatMap<std::string, int> move_assigned;
    move_assigned = std::move(moved);
    return moved.empty() && move_assigned.size() == 3 && move_assigned.at("alpha") == 1;
}

bool preserves_storage_when_copy_growth_fails() {
    FlatMap<int, CopyPreferred> map;
    map.try_emplace(1, 10);
    map.try_emplace(2, 20);
    map.try_emplace(3, 30);

    const auto *old_keys = map.keys_data();
    const auto *old_values = map.values_data();
    const auto old_capacity = map.capacity();
    CopyPreferred::fail_copy = true;
    try {
        map.reserve(old_capacity + 20);
        CopyPreferred::fail_copy = false;
        return false;
    } catch (const std::runtime_error &) {
        CopyPreferred::fail_copy = false;
    }

    if (map.keys_data() != old_keys || map.values_data() != old_values || map.capacity() != old_capacity || map.size() != 3 ||
        map.at(1).value != 10 || map.at(2).value != 20 || map.at(3).value != 30)
        return false;

    map.erase(2);
    return map.size() == 2 && map.begin()->first == 1 && map.begin()->second.value == 10 && (map.begin() + 1)->first == 3 &&
           (map.begin() + 1)->second.value == 30;
}

bool releases_both_allocations() {
    const usize allocations_before = live_allocations.load(std::memory_order_relaxed);
    {
        FlatMap<int, std::unique_ptr<int>> map;
        for (int value = 0; value < 128; ++value) map.try_emplace(value, std::make_unique<int>(value));
        map.erase(map.begin() + 20, map.begin() + 80);
        map.reserve(512);
        map.shrink_to_fit();
        map.clear();
    }
    return live_allocations.load(std::memory_order_relaxed) == allocations_before;
}

bool reports_invalid_access() {
    FlatMap<int, int> map;
    try {
        static_cast<void>(map.at(2));
        return false;
    } catch (const std::out_of_range &) {
        return true;
    }
}

} // namespace

int main() {
#define RUN_TEST(test)                  \
    if (!(test())) {                    \
        std::cerr << #test " failed\n"; \
        return 1;                       \
    }

    RUN_TEST(modifies_and_erases)
    RUN_TEST(grows_without_a_fixed_capacity)
    RUN_TEST(bulk_constructs_unsorted_ranges)
    RUN_TEST(uses_parallel_storage)
    RUN_TEST(owns_and_relocates_entries)
    RUN_TEST(copies_and_moves_storage)
    RUN_TEST(preserves_storage_when_copy_growth_fails)
    RUN_TEST(releases_both_allocations)
    RUN_TEST(reports_invalid_access)

#undef RUN_TEST
    return 0;
}
