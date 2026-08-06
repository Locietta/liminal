#include <concepts>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <proxy/proxy.h>

#include <lighter/mock/mock.h>

namespace {

using namespace lighter;

struct Service {
    int count = 0;
    std::unique_ptr<int> internal_data;

    int transform(int) const;
    void notify(std::string_view) const;
    std::unique_ptr<int> acquire() const;
};

PRO_DEF_MEM_DISPATCH(TransformDispatch, transform);
PRO_DEF_MEM_DISPATCH(NotifyDispatch, notify);
PRO_DEF_MEM_DISPATCH(NoexceptTransformDispatch, transform);

struct ServiceFacade
    : pro::facade_builder::add_convention<TransformDispatch, int(int) const>::add_convention<NotifyDispatch,
                                                                                             void(std::string_view) const>::build {};

struct NoexceptServiceFacade : pro::facade_builder::add_convention<NoexceptTransformDispatch, int(int) const noexcept>::build {};

template <typename T>
concept ServiceLike = requires(const T &service) {
    { service.transform(1) } -> std::same_as<int>;
    service.notify("message");
    { service.acquire() } -> std::same_as<std::unique_ptr<int>>;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

int use_service(const ServiceLike auto &service, int input) {
    service.notify("transforming");
    return service.transform(input);
}

void test_behavior_and_verification() {
    mock::Mock<Service> service;
    service.expect<^^Service::transform>().calls([](int value) { return value * 2; }).times(2);
    service.expect<^^Service::notify>()
        .calls([](std::string_view message) { require(message == "transforming", "mock received the wrong argument"); })
        .times(2);

    auto handle = service.handle();
    require(use_service(handle, 3) == 6, "mock returned the wrong first result");
    require(use_service(handle, 4) == 8, "mock returned the wrong second result");
    require(service.expect<^^Service::transform>().call_count() == 2, "mock recorded the wrong call count");
    service.verify();
}

void test_fixed_return() {
    mock::Mock<Service> service;
    service.expect<^^Service::transform>().returns(42);
    service.expect<^^Service::notify>().returns();

    require(use_service(service.handle(), 7) == 42, "fixed return behavior failed");
    service.verify();
}

void test_unexpected_call() {
    mock::Mock<Service> service;
    bool rejected = false;
    try {
        (void) service.handle().transform(1);
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("transform");
    }
    require(rejected, "an unconfigured call must fail with the reflected member name");
}

void test_missing_call() {
    mock::Mock<Service> service;
    service.expect<^^Service::transform>().returns(1).once();

    bool rejected = false;
    try {
        service.verify();
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("transform");
    }
    require(rejected, "verification must reject a missing call");
}

void test_never() {
    mock::Mock<Service> service;
    service.expect<^^Service::transform>().never();

    bool rejected = false;
    try {
        (void) service.handle().transform(1);
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("expected 0 call");
    }
    require(rejected, "a forbidden call must fail immediately");
}

void test_allowed_behavior_is_unrestricted() {
    mock::Mock<Service> service;
    service.allow<^^Service::transform>().calls([](int value) { return value + 1; });
    auto handle = service.handle();

    require(handle.transform(1) == 2, "allowed behavior returned the wrong first value");
    require(handle.transform(4) == 5, "allowed behavior returned the wrong second value");
    service.verify();
}

void test_ordered_and_move_only_results() {
    mock::Mock<Service> service;
    service.expect<^^Service::transform>().then_returns(3).then_calls([](int value) { return value * 2; });
    service.expect<^^Service::acquire>().then_returns(std::make_unique<int>(9));
    auto handle = service.handle();

    require(handle.transform(100) == 3, "ordered fixed result was not used first");
    require(handle.transform(4) == 8, "ordered callable result was not used second");
    auto acquired = handle.acquire();
    require(acquired && *acquired == 9, "move-only result was not returned");
    service.verify();
}

void test_handle_outlives_controller() {
    auto handle = [] {
        mock::Mock<Service> service;
        service.allow<^^Service::transform>().returns(17);
        return service.handle();
    }();

    require(handle.transform(0) == 17, "copied handle did not retain its state");
}

void test_mock_is_movable() {
    mock::Mock<Service> original;
    original.expect<^^Service::transform>().returns(5);
    auto moved = std::move(original);

    require(moved.handle().transform(0) == 5, "moved mock lost its dispatcher state");
    moved.verify();
}

void test_verification_reports_all_failures() {
    mock::Mock<Service> service;
    service.expect<^^Service::transform>().returns(1);
    service.expect<^^Service::notify>().returns();

    bool complete = false;
    try {
        service.verify();
    } catch (const mock::Error &error) {
        auto message = std::string_view(error.what());
        complete = message.contains("transform") && message.contains("notify");
    }
    require(complete, "verification must report every unmet expectation");
}

void test_automatic_port() {
    mock::Mock<Service> service;
    service.expect<^^Service::transform>().calls([](int value) { return value + 2; });
    service.expect<^^Service::notify>().returns();
    service.expect<^^Service::acquire>().then_returns(std::make_unique<int>(8));
    auto handle = service.handle();

    handle.notify("generated");
    require(handle.transform(3) == 5, "generated port lost the reflected method signature");
    auto acquired = handle.acquire();
    require(acquired && *acquired == 8, "generated port lost the reflected move-only result");
    service.verify();
}

void test_facade_contract() {
    mock::Mock<ServiceFacade> service;
    service.expect<TransformDispatch>().calls([](int value) { return value + 4; });
    service.expect<NotifyDispatch>().calls(
        [](std::string_view message) { require(message == "facade", "facade mock received the wrong argument"); });

    auto proxy = pro::make_proxy<ServiceFacade>(service.handle());
    proxy->notify("facade");
    require(proxy->transform(3) == 7, "facade mock returned the wrong value");
    service.verify();
}

void test_noexcept_facade_contract() {
    mock::Mock<NoexceptServiceFacade> service;
    service.expect<NoexceptTransformDispatch>().calls([](int value) noexcept { return value * 3; });

    auto proxy = pro::make_proxy<NoexceptServiceFacade>(service.handle());
    static_assert(noexcept(proxy->transform(2)));
    require(proxy->transform(2) == 6, "noexcept facade mock returned the wrong value");
    service.verify();
}

void test_noexcept_failure_is_deferred() {
    mock::Mock<NoexceptServiceFacade> service;
    service.expect<NoexceptTransformDispatch>().never();
    auto proxy = pro::make_proxy<NoexceptServiceFacade>(service.handle());

    require(proxy->transform(2) == 0, "unconfigured noexcept mock did not return its safe fallback");
    bool rejected = false;
    try {
        service.verify();
    } catch (const mock::Error &error) {
        rejected = std::string_view(error.what()).contains("unexpected call");
    }
    require(rejected, "noexcept mock did not defer its failure to verification");
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
    test_automatic_port();
    test_facade_contract();
    test_noexcept_facade_contract();
    test_noexcept_failure_is_deferred();
}
