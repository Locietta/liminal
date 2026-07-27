This is a concise code style guide for the Liminal project. Contributors or agents should follow these guidelines unless there is a compelling reason to deviate.

If not specified here, just follow the code style of the existing code around the code you are writing.

## Identifier Naming

See [.clang-tidy](../.clang-tidy).

## Header Inclusion

The headers should be grouped with the following order:

```c++
/// interface header of the current .cpp file
#include "interface.h"

// std headers
#include <cstdlib>
#include <vector>

// other 3rd party headers
#include <fmt/format.h>

// other headers from the Liminal project
#include <lighter/http/client.h>
#include <lighter/async/async.h>
```

## Structs, Custom Types

Always use `struct`, do not use `class` for custom types. This way, we don't need to write `public:` for every struct, and when we need to inherit, we don't need to write `public` in the inheritance list.

## Polymorphism

We generally avoid virtual functions in this project. If you need polymorphism, use `ngcpp-proxy` (previously `microsoft-proxy`). It provide a dynamic dispatch mechanism based on fat pointers, much close to rust's trait objects. It can also support static dispatch with `PRO_DEF_MEM_DISPATCH` and `PRO_DEF_FREE_DISPATCH`. An example:

```c++
struct RenderContext {};

PRO_DEF_MEM_DISPATCH(DrawDispatch, draw); // generate a templated dispatch funtion draw at global scope

inline constexpr DrawDispatch draw{};

struct DrawableFacade
    : pro::facade_builder
        ::add_convention<
            DrawDispatch,
            void(RenderContext&) const>
        ::build
{};

struct Mesh {
    void draw(RenderContext&) const
    {
        // ...
    }
};

template<class T>
concept Drawable = requires(
    const T& value,
    RenderContext& ctx)
{
    { draw(value, ctx) } -> std::same_as<void>;
};

void render_static(
    const Drawable auto& value,
    RenderContext& ctx)
{
    draw(value, ctx);        // statically dispatched
}

void render_dynamic(
    const pro::proxy<DrawableFacade>& value,
    RenderContext& ctx)
{
    draw(*value, ctx);       // dynamically dispatched by Proxy
}
```

This way we can keep the main structs in our codebase as plain non virtual structs, with better memory layout and data locality, and still have polymorphism when needed.

## Basic Types

We have some short convenience types defined in `lighter/types.hpp`, use them over the standard types. (e.g. use `usize` instead of `std::size_t`).

These types should be direcly available inside the `lighter` namespace, if you are in a different namespace, `using namespace lighter::types;` is generally recommended.
