add_requires("ngcpp-proxy")

target("liminal-core")
    set_kind("static")
    add_files("agent/*.cpp", "provider/*.cpp", "tools/*.cpp", "tui/*.cpp")
    add_deps("lighter")
    add_packages("ngcpp-proxy", {public = true})

target("liminal")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("liminal-core")

includes("tests/xmake.lua")
