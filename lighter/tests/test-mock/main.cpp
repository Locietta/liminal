#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <lighter/mock/mock.h>

namespace {

using namespace lighter;

struct ServicePort {
    std::copyable_function<int(int) const> transform;
    std::copyable_function<void(std::string_view)> notify;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

int use_service(ServicePort &service, int input) {
    service.notify("transforming");
    return service.transform(input);
}

void test_behavior_and_verification() {
    mock::Mock<ServicePort> service;
    service.on<^^ServicePort::transform>().calls([](int value) { return value * 2; }).times(2);
    service.on<^^ServicePort::notify>()
        .calls([](std::string_view message) { require(message == "transforming", "mock received the wrong argument"); })
        .times(2);

    require(use_service(service.object(), 3) == 6, "mock returned the wrong first result");
    require(use_service(service.object(), 4) == 8, "mock returned the wrong second result");
    require(service.on<^^ServicePort::transform>().call_count() == 2, "mock recorded the wrong call count");
    service.verify();
}

void test_fixed_return() {
    mock::Mock<ServicePort> service;
    service.on<^^ServicePort::transform>().returns(42);
    service.on<^^ServicePort::notify>().returns();

    require(use_service(service.object(), 7) == 42, "fixed return behavior failed");
    service.verify();
}

void test_unexpected_call() {
    mock::Mock<ServicePort> service;
    bool rejected = false;
    try {
        (void) service.object().transform(1);
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("transform");
    }
    require(rejected, "an unconfigured call must fail with the reflected member name");
}

void test_missing_call() {
    mock::Mock<ServicePort> service;
    service.on<^^ServicePort::transform>().returns(1).once();

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
    service.on<^^ServicePort::transform>().never();

    bool rejected = false;
    try {
        (void) service.object().transform(1);
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("expected 0 call");
    }
    require(rejected, "a forbidden call must fail immediately");
}

} // namespace

int main() {
    test_behavior_and_verification();
    test_fixed_return();
    test_unexpected_call();
    test_missing_call();
    test_never();
}
