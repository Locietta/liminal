# Reflection-driven mocking

`lighter::mock::Mock<Contract>` generates a callable mock port from either a
plain method contract or an `ngcpp-proxy` facade. The user declares each
operation only once and refers to that declaration when configuring behavior.
Ordinary data members on a plain contract are ignored.

```cpp
struct Files {
    int open_count = 0; // Not part of the generated port.

    std::string read(std::string_view path) const;
    void remove(std::string_view path) const;
};

lighter::mock::Mock<Files> files;
files.expect<^^Files::read>().returns("contents");
files.expect<^^Files::remove>().never();

run(files.handle());
files.verify();
```

The methods need declarations but not definitions. `Mock<Files>::Port` is a
generated aggregate with `read` and `remove` callable fields, normally backed
by `std::copyable_function`. Its call syntax matches the declared methods, so
it fits structurally injected dependencies and `ngcpp-proxy` targets. It is not
a `Files` object and cannot replace a concrete parameter of type `Files&`.

For an `ngcpp-proxy` facade, pass the facade directly and select operations by
their dispatch types. No parallel method-contract struct is needed:

```cpp
PRO_DEF_MEM_DISPATCH(ReadDispatch, read);
PRO_DEF_MEM_DISPATCH(RemoveDispatch, remove);

struct FilesFacade
    : pro::facade_builder
          ::add_convention<ReadDispatch,
                           std::string(std::string_view) const>
          ::add_convention<RemoveDispatch,
                           void(std::string_view) const>
          ::build {};

lighter::mock::Mock<FilesFacade> files;
files.expect<ReadDispatch>().returns("contents");
files.expect<RemoveDispatch>().never();

auto provider = pro::make_proxy<FilesFacade>(files.handle());
provider->read("settings.json");
files.verify();
```

`Mock` reflects `FilesFacade::convention_types`, obtaining the operation name
and signature from each dispatch accessor. The generated port therefore
satisfies the facade without duplicating its declarations.

Use `calls` when behavior depends on arguments or the result is move-only:

```cpp
files.expect<^^Files::read>().calls([](std::string_view path) {
    if (path != "settings.json") {
        throw std::runtime_error("unexpected path");
    }
    return std::string("{}");
});
```

`expect` expects one call by default. Override that with `times`, `once`, or
`never`. Use `allow` to install behavior without constraining the call count.
Ordered `then_calls` and `then_returns` actions infer their expected count and
support one-shot move-only results. `verify` reports every unmet expectation.

`handle()` returns an owning port value. Its dispatchers share ownership of
their state, so handles may outlive the movable mock controller. A normal port
can also be copied independently.

`Port` is intentionally an implementation detail of `Mock`. Production APIs
should express their boundary as a facade or as a constrained generic
interface rather than naming the generated function table. A generated port
can satisfy such a structural interface or be stored in an `ngcpp-proxy`.

## Design boundaries

- Plain contracts and facades require one non-overloaded ordinary member
  operation per name. Facades support indirect member conventions. A direct
  convention operates on proxy's stored pointer wrapper rather than the
  pointee, while a free convention requires an actual ADL-visible function;
  neither can be represented by `define_aggregate` data members in C++26.
- `noexcept` operations are supported when their result is `void` or
  nothrow-default-constructible. Their `calls` and `then_calls`
  implementations must be nothrow-invocable. Because an exception cannot
  escape such a call, immediate mock violations are recorded and reported by
  `verify()`; an unconfigured non-void call returns `R{}` in the meantime.
- Volatile and ref-qualified operations are rejected because the generated
  aggregate needs one callable field per operation.
- `std::function_ref` is deliberately not used for port fields. It is
  non-owning and non-default-constructible, so a returned or coroutine-stored
  handle could outlive the controller and dangle.
- Port dispatchers use `std::copyable_function`. Move-only configured behavior
  is still supported because actions are owned separately by the mock state;
  making the generated handle itself move-only would add restrictions without
  enabling another use case.
- `returns(value)` requires a non-reference, copyable result because the same
  behavior may run repeatedly. `then_returns(value)` accepts move-only results
  because each ordered action runs once. Use callables for reference results.
- Arguments are not copied into an implicit history. Assert on them inside
  `calls`; this avoids surprising copies, dangling references, and loss of
  move-only arguments.
- Verification is explicit. Destructors do not throw and do not hide failures
  during stack unwinding.

C++26 reflection still cannot synthesize member functions. The generated port
therefore remains an aggregate of callable data members; `define_aggregate`
cannot create a drop-in subclass or concrete implementation of `Contract`.

## Standards and compiler context

This implementation targets the repository's pinned GCC 16.1 and P2996R13
reflection surface.

- [P2996R13, Reflection for C++26](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2996r13.html)
- [P3157R1, Generative Extensions for Reflection](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3157r1.html)
- [GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html)
