#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <lighter/mock/mock.h>

namespace {

using namespace lighter;

struct ServicePort {
    std::copyable_function<int(int) const> transform;
    std::copyable_function<void(std::string_view) const> notify;
    std::copyable_function<std::unique_ptr<int>() const> acquire;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

int use_service(const ServicePort &service, int input) {
    service.notify("transforming");
    return service.transform(input);
}

void test_behavior_and_verification() {
    mock::Mock<ServicePort> service;
    service.expect<^^ServicePort::transform>().calls([](int value) { return value * 2; }).times(2);
    service.expect<^^ServicePort::notify>()
        .calls([](std::string_view message) { require(message == "transforming", "mock received the wrong argument"); })
        .times(2);

    auto handle = service.handle();
    require(use_service(handle, 3) == 6, "mock returned the wrong first result");
    require(use_service(handle, 4) == 8, "mock returned the wrong second result");
    require(service.expect<^^ServicePort::transform>().call_count() == 2, "mock recorded the wrong call count");
    service.verify();
}

void test_fixed_return() {
    mock::Mock<ServicePort> service;
    service.expect<^^ServicePort::transform>().returns(42);
    service.expect<^^ServicePort::notify>().returns();

    require(use_service(service.handle(), 7) == 42, "fixed return behavior failed");
    service.verify();
}

void test_unexpected_call() {
    mock::Mock<ServicePort> service;
    bool rejected = false;
    try {
        (void) service.handle().transform(1);
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("transform");
    }
    require(rejected, "an unconfigured call must fail with the reflected member name");
}

void test_missing_call() {
    mock::Mock<ServicePort> service;
    service.expect<^^ServicePort::transform>().returns(1).once();

    bool rejected = false;
    try {
        service.verify();
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("transform");
    }
    require(rejected, "verification must reject a missing call");
}

void test_never() {
    mock::Mock<ServicePort> service;
    service.expect<^^ServicePort::transform>().never();

    bool rejected = false;
    try {
        (void) service.handle().transform(1);
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("expected 0 call");
    }
    require(rejected, "a forbidden call must fail immediately");
}

void test_allowed_behavior_is_unrestricted() {
    mock::Mock<ServicePort> service;
    service.allow<^^ServicePort::transform>().calls([](int value) { return value + 1; });
    auto handle = service.handle();

    require(handle.transform(1) == 2, "allowed behavior returned the wrong first value");
    require(handle.transform(4) == 5, "allowed behavior returned the wrong second value");
    service.verify();
}

void test_ordered_and_move_only_results() {
    mock::Mock<ServicePort> service;
    service.expect<^^ServicePort::transform>().then_returns(3).then_calls([](int value) { return value * 2; });
    service.expect<^^ServicePort::acquire>().then_returns(std::make_unique<int>(9));
    auto handle = service.handle();

    require(handle.transform(100) == 3, "ordered fixed result was not used first");
    require(handle.transform(4) == 8, "ordered callable result was not used second");
    auto acquired = handle.acquire();
    require(acquired && *acquired == 9, "move-only result was not returned");
    service.verify();
}

void test_handle_outlives_controller() {
    ServicePort handle = [] {
        mock::Mock<ServicePort> service;
        service.allow<^^ServicePort::transform>().returns(17);
        return service.handle();
    }();

    require(handle.transform(0) == 17, "copied handle did not retain its state");
}

void test_mock_is_movable() {
    mock::Mock<ServicePort> original;
    original.expect<^^ServicePort::transform>().returns(5);
    auto moved = std::move(original);

    require(moved.handle().transform(0) == 5, "moved mock lost its dispatcher state");
    moved.verify();
}

void test_verification_reports_all_failures() {
    mock::Mock<ServicePort> service;
    service.expect<^^ServicePort::transform>().returns(1);
    service.expect<^^ServicePort::notify>().returns();

    bool complete = false;
    try {
        service.verify();
    } catch (const mock::Error &error) {
        auto message = std::string_view(error.what());
        complete = message.contains("transform") && message.contains("notify");
    }
    require(complete, "verification must report every unmet expectation");
}

} // namespace

int main() {
    test_behavior_and_verification();
    test_fixed_return();
    test_unexpected_call();
    test_missing_call();
    test_never();
    test_allowed_behavior_is_unrestricted();
    test_ordered_and_move_only_results();
    test_handle_outlives_controller();
    test_mock_is_movable();
    test_verification_reports_all_failures();
}
