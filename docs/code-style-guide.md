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

## Basic Types

We have some short convenience types defined in `lighter/types.hpp`, use them over the standard types. (e.g. use `usize` instead of `std::size_t`).

These types should be direcly available inside the `lighter` namespace, if you are in a different namespace, `using namespace lighter::types;` is generally recommended.
