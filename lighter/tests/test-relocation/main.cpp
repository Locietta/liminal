#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/utils/functional.h>
#include <lighter/utils/relocation.h>
#include <lighter/utils/small_vector.h>

namespace {

using namespace lighter;
using mem::is_trivially_relocatable_v;
using mem::trivially_relocatable;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

// --- trait detection: static surface ----------------------------------------

struct Aggregate {
    int a;
    double b;
    void *p;
};

struct Nested {
    Aggregate inner;
    std::unique_ptr<int> owner;
};

struct DerivedOwner : Aggregate {
    std::unique_ptr<int> owner;
};

struct UserMove {
    UserMove(UserMove &&other) noexcept : x(other.x) {}
    int x = 0;
};

struct SelfReferential {
    SelfReferential() = default;
    SelfReferential(SelfReferential &&) noexcept : self(this) {}
    SelfReferential *self = this;
};

struct Polymorphic {
    virtual ~Polymorphic() = default;
    int x = 0;
};

struct[[= trivially_relocatable]] ForcedOn {
    ForcedOn() = default;
    ForcedOn(ForcedOn &&other) noexcept : x(other.x) { other.x = 0; }
    int x = 0;
};

// Non-trivially-copyable (unique_ptr member) so the heuristic would say true;
// the annotation vetoes it.
struct[[= trivially_relocatable(false)]] ForcedOff {
    std::unique_ptr<int> p;
};

struct HoldsForcedOff {
    ForcedOff member;
};

// The opt-out must also work on a trivially copyable type - annotations
// outrank the TC fast path - and poison enclosing types (even TC ones).
struct[[= trivially_relocatable(false)]] ForcedOffTc {
    int x;
};

struct HoldsForcedOffTc {
    ForcedOffTc member;
};

static_assert(std::is_trivially_copyable_v<ForcedOffTc>);
static_assert(std::is_trivially_copyable_v<HoldsForcedOffTc>);

// scalars and aggregates thereof
static_assert(is_trivially_relocatable_v<int>);
static_assert(is_trivially_relocatable_v<void *>);
static_assert(is_trivially_relocatable_v<int[8]>);
static_assert(is_trivially_relocatable_v<const Aggregate>);

// rule-of-zero composition through members and bases, incl. unique_ptr
static_assert(is_trivially_relocatable_v<std::unique_ptr<int>>);
static_assert(is_trivially_relocatable_v<Nested>);
static_assert(is_trivially_relocatable_v<DerivedOwner>);
static_assert(is_trivially_relocatable_v<std::shared_ptr<int>>);

// lambdas: capturing relocatable state is relocatable
constexpr auto probe_lambda = [p = std::unique_ptr<int>{}, n = 7]() { return n + (p ? *p : 0); };
static_assert(is_trivially_relocatable_v<std::remove_cv_t<decltype(probe_lambda)>>);

// conservative denials
static_assert(!is_trivially_relocatable_v<UserMove>);
static_assert(!is_trivially_relocatable_v<SelfReferential>);
static_assert(!is_trivially_relocatable_v<Polymorphic>);
static_assert(!is_trivially_relocatable_v<std::string>); // SSO interior pointer (libstdc++)

// annotations override in both directions, and propagate through membership
static_assert(is_trivially_relocatable_v<ForcedOn>);
static_assert(!is_trivially_relocatable_v<ForcedOff>);
static_assert(!is_trivially_relocatable_v<HoldsForcedOff>);
static_assert(!is_trivially_relocatable_v<ForcedOffTc>);
static_assert(!is_trivially_relocatable_v<HoldsForcedOffTc>);

// lighter's own types
static_assert(!is_trivially_relocatable_v<SmallVector<int>>); // inline storage is address-sensitive

// --- allocation counting ----------------------------------------------------

usize live_allocations = 0;
usize total_allocations = 0;

} // namespace

void *operator new(std::size_t size) {
    ++live_allocations;
    ++total_allocations;
    if (void *p = std::malloc(size)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void *p) noexcept {
    if (p) {
        --live_allocations;
    }
    std::free(p);
}

void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }

namespace {

// --- runtime: SmallVector relocates ownership correctly ---------------------

void test_small_vector_unique_ptr_growth() {
    SmallVector<std::unique_ptr<i32>, 2> owners;
    for (i32 i = 0; i < 100; ++i) {
        owners.emplace_back(std::make_unique<i32>(i));
    }

    require(owners.size() == 100, "growth lost elements");
    for (i32 i = 0; i < 100; ++i) {
        require(owners[static_cast<usize>(i)] != nullptr && *owners[static_cast<usize>(i)] == i,
                "unique_ptr payload corrupted by relocation");
    }

    // insert in the middle forces the relocate-prefix/construct/relocate-suffix path
    owners.insert(owners.begin() + 50, std::make_unique<i32>(-1));
    require(owners.size() == 101 && *owners[50] == -1 && *owners[51] == 50, "middle insert misplaced elements");

    owners.clear();
}

void test_small_vector_shrink_to_fit_inline() {
    SmallVector<std::unique_ptr<i32>, 8> owners;
    for (i32 i = 0; i < 32; ++i) {
        owners.emplace_back(std::make_unique<i32>(i));
    }
    owners.truncate(4);
    owners.shrink_to_fit(); // heap -> inline relocation
    require(owners.inlined(), "shrink_to_fit did not return to inline storage");
    for (i32 i = 0; i < 4; ++i) {
        require(*owners[static_cast<usize>(i)] == i, "inline relocation corrupted elements");
    }
}

void test_leak_free() {
    const auto live_before = live_allocations;
    {
        SmallVector<std::unique_ptr<i32>, 2> owners;
        for (i32 i = 0; i < 64; ++i) {
            owners.emplace_back(std::make_unique<i32>(i));
        }
        owners.erase(owners.begin() + 10, owners.begin() + 20);
        owners.insert(owners.begin() + 5, std::make_unique<i32>(1000));
        owners.shrink_to_fit();
    }
    const bool leak_free = live_allocations == live_before;
    require(leak_free, "relocation paths leaked or double-freed");
}

// --- runtime: Function SBO admits move-only relocatable callables -----------

void test_function_sbo_unique_ptr_capture() {
    auto payload = std::make_unique<i32>(42);

    const auto total_before_capture = total_allocations;
    // the capture itself was allocated above; constructing the Function must
    // add nothing (SBO), where it previously heap-allocated the closure
    Function<i32()> fn = [p = std::move(payload)]() { return *p; };
    const auto function_allocs = total_allocations - total_before_capture;
    require(function_allocs == 0, "unique_ptr-capturing lambda was heap-allocated instead of using SBO");

    require(fn() == 42, "Function SBO callable returned wrong value");

    // moving the Function relocates the closure bytewise; ownership must follow
    Function<i32()> moved = std::move(fn);
    require(moved() == 42, "moved Function lost its captured state");
}

void test_function_sbo_still_rejects_oversized() {
    struct Big {
        byte blob[64];
        i32 tag;
    };
    const auto total_before = total_allocations;
    Function<i32()> fn = [big = Big{.blob = {}, .tag = 7}]() { return big.tag; };
    require(total_allocations > total_before, "oversized callable unexpectedly fit in SBO");
    require(fn() == 7, "heap-allocated callable returned wrong value");
}

} // namespace

int main() {
    test_small_vector_unique_ptr_growth();
    test_small_vector_shrink_to_fit_inline();
    test_leak_free();
    test_function_sbo_unique_ptr_capture();
    test_function_sbo_still_rejects_oversized();
    return 0;
}
