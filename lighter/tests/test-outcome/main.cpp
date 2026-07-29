#include <concepts>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <lighter/async/vocab/outcome.h>

namespace {

using namespace lighter;

using TestOutcome = Outcome<std::string, int, bool>;

static_assert(std::same_as<decltype(std::declval<TestOutcome &>().value()), std::string &>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &>().value()), const std::string &>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &&>().value()), std::string &&>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &&>().value()), const std::string &&>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &>().error()), int &>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &>().error()), const int &>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &&>().error()), int &&>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &&>().error()), const int &&>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &>().cancellation()), bool &>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &>().cancellation()), const bool &>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &&>().cancellation()), bool &&>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &&>().cancellation()), const bool &&>);

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

void test_value() {
    Outcome<std::string, int, bool> outcome("value");
    require(outcome.has_value(), "value outcome has the wrong state");
    require(outcome.value() == "value", "value accessor returned the wrong value");
    require(*outcome == "value", "dereference returned the wrong value");
    require(outcome->size() == 5, "arrow returned the wrong value");
}

void test_error() {
    Outcome<std::string, int, bool> outcome(outcome_error(42));
    require(outcome.has_error(), "error outcome has the wrong state");
    require(outcome.error() == 42, "error accessor returned the wrong error");
}

void test_cancellation() {
    Outcome<std::string, int, bool> outcome(outcome_cancel(true));
    require(outcome.is_cancelled(), "cancelled outcome has the wrong state");
    require(outcome.cancellation(), "cancellation accessor returned the wrong value");
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--violate") {
        TestOutcome outcome(outcome_error(42));
        static_cast<void>(outcome.value());
        return 1;
    }

    test_value();
    test_error();
    test_cancellation();
    return 0;
}
