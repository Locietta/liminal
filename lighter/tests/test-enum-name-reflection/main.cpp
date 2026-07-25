#include <string_view>

#include <lighter/utils/enum.h>

namespace {

using namespace lighter;

enum struct Snake {
    MUTEX_WAITER,
    HTTP_SERVER,
    A,
};

enum struct Camel {
    MutexWaiter,
    HTTPServer,
    parseJSON,
    A,
};

enum struct Lower {
    mutex_waiter,
    a,
};

enum struct Sparse : int {
    FIRST = -3,
    SECOND = 42,
    THIRD = 42 + 1,
};

// verbatim (NO_CHANGE default)
static_assert(enum_name(Snake::MUTEX_WAITER) == "MUTEX_WAITER");
static_assert(enum_name(Camel::parseJSON) == "parseJSON");
static_assert(enum_name(Lower::a) == "a");

// non-contiguous and negative values resolve by value, not by index
static_assert(enum_name(Sparse::FIRST) == "FIRST");
static_assert(enum_name(Sparse::SECOND) == "SECOND");
static_assert(enum_name(Sparse::THIRD) == "THIRD");

// unmatched values: default and custom fallback, passed through unconverted
static_assert(enum_name(static_cast<Snake>(99)) == "Unknown");
static_assert(enum_name<NameCase::UPPER>(static_cast<Snake>(99), "n/a") == "n/a");

// UPPER source -> full matrix
static_assert(enum_name<NameCase::CAMEL>(Snake::MUTEX_WAITER) == "MutexWaiter");
static_assert(enum_name<NameCase::LOWER>(Snake::MUTEX_WAITER) == "mutex_waiter");
static_assert(enum_name<NameCase::UPPER>(Snake::MUTEX_WAITER) == "MUTEX_WAITER");

// Camel source -> full matrix, including acronym runs
static_assert(enum_name<NameCase::LOWER>(Camel::MutexWaiter) == "mutex_waiter");
static_assert(enum_name<NameCase::UPPER>(Camel::MutexWaiter) == "MUTEX_WAITER");
static_assert(enum_name<NameCase::LOWER>(Camel::HTTPServer) == "http_server");
static_assert(enum_name<NameCase::UPPER>(Camel::parseJSON) == "PARSE_JSON");
// acronym casing flattens on the CamelCase rendering (documented behavior)
static_assert(enum_name<NameCase::CAMEL>(Camel::HTTPServer) == "HttpServer");

// lower source -> full matrix
static_assert(enum_name<NameCase::CAMEL>(Lower::mutex_waiter) == "MutexWaiter");
static_assert(enum_name<NameCase::UPPER>(Lower::mutex_waiter) == "MUTEX_WAITER");

// single-word / single-letter edge cases
static_assert(enum_name<NameCase::CAMEL>(Snake::A) == "A");
static_assert(enum_name<NameCase::LOWER>(Camel::A) == "a");
static_assert(enum_name<NameCase::UPPER>(Lower::a) == "A");

// converted names are usable at runtime (point into static storage)
std::string_view runtime_name(Snake value) { return enum_name<NameCase::CAMEL>(value); }

} // namespace

int main() {
    // the constexpr surface above is the test; exercise one runtime path so
    // the static-storage promotion is covered by an actual call
    return runtime_name(Snake::HTTP_SERVER) == "HttpServer" ? 0 : 1;
}
