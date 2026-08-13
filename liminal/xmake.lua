add_requires("boost_uuid 1.91.0", "libunicode 0.9.2", "ngcpp-proxy", "sqlite3 >=3.51.3 <4")

target("liminal-core")
    set_kind("static")
    add_rules("utils.bin2obj.cpp", {extensions = {".md"}})
    add_files("agent/*.cpp", "context/*.cpp", "model/*.cpp", "provider/*.cpp", "provider/detail/*.cpp", "session/*.cpp", "tools/*.cpp", "tui/*.cpp")
    add_files("prompts/default-agent.md", "prompts/runtime-tools.md")
    add_deps("lighter")
    add_packages("boost_uuid", "sqlite3")
    add_packages("libunicode", "ngcpp-proxy", {public = true})

target("liminal")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("liminal-core")

includes("tests/xmake.lua")
