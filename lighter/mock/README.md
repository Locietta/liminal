# Reflection-driven mocking

## Recommendation

C++26 reflection can remove the handwritten fake for a **callable port**: a
plain struct whose public data members are `std::copyable_function` operations.
`lighter::mock::Mock<Port>` reflects those members, binds a correctly typed
dispatcher to each one, and uses the reflected member name in failures.

```cpp
struct Files {
    std::copyable_function<std::string(std::string_view) const> read;
    std::copyable_function<void(std::string_view) const> remove;
};

lighter::mock::Mock<Files> files;
files.on<^^Files::read>().returns("contents").once();
files.on<^^Files::remove>().never();

run(files.object());
files.verify();
```

Use `calls` when behavior depends on arguments or the result is move-only:

```cpp
files.on<^^Files::read>().calls([](std::string_view path) {
    if (path != "settings.json") {
        throw std::runtime_error("unexpected path");
    }
    return std::string("{}");
});
```

`calls` and `returns` expect one call by default. Override that with `times`,
`once`, or `never`. `verify` checks expectations that were not already rejected
at invocation time. The mock is deliberately non-movable because its generated
dispatchers point back to it.

## Why this shape

C++26 reflection is strong at introspection: it can enumerate a type's members,
recover their types and identifiers, and splice an existing member into an
expression. It cannot synthesize member functions. `define_aggregate` only
defines data members, so the standard facilities cannot generate a drop-in fake
for a conventional virtual interface, a plain struct with methods, or an
`ngcpp-proxy` facade.

Function synthesis has been proposed separately in P3157, but it is not part of
C++26 or GCC 16. A method-generating mock should therefore not be the project's
foundation yet.

## Design boundaries

- A port must be default-constructible and contain only direct, public
  `std::copyable_function<R(Args...)>` members. Both unqualified and `const`
  signatures are accepted; `noexcept` is intentionally rejected because mock
  failures throw.
- `returns(value)` requires a non-reference, copyable result because the same
  behavior may run repeatedly. Use `calls` for references and move-only values.
- Arguments are not copied into an implicit history. Assert on them inside
  `calls`; that avoids surprising copies, dangling references, and loss of
  move-only arguments.
- Verification is explicit. Destructors do not throw and do not hide a failure
  during stack unwinding.
- This is a good fit for dependency boundaries that are naturally function
  tables. Keep `ngcpp-proxy` for open-ended runtime polymorphism and use small
  handwritten fakes there until function synthesis becomes standardized.

## Potential next steps

Adopt callable ports on one dependency boundary with substantial fake
boilerplate, then evaluate readability and compile-time cost. If the pattern is
useful, useful extensions are ordered behaviors and opt-in argument capture.
Avoid automatic deep equality, matcher DSLs, and destructor verification until
real tests demonstrate a need.

## Standards and compiler context

This prototype targets the repository's pinned GCC 16.1 and P2996R13 reflection
surface. Relevant upstream material:

- [P2996R13, Reflection for C++26](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2996r13.html)
- [P3157R1, Generative Extensions for Reflection](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3157r1.html)
- [GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html)
