add_requires("pixi::libuv", {alias = "libuv"})
add_requires("pixi::fmt", {alias = "fmt"})

target("lighter")
    set_kind("static")
    add_files("**/*.cpp")
    add_packages("libuv", "fmt")
