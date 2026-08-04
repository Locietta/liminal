add_requires("libunicode 0.9.2", "ngcpp-proxy")

target("liminal-core")
    set_kind("static")
    add_files("agent/*.cpp", "model/*.cpp", "provider/*.cpp", "tools/*.cpp", "tui/*.cpp")
    add_deps("lighter")
    add_packages("libunicode", "ngcpp-proxy", {public = true})

target("liminal")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("liminal-core")

target("liminal-dev-mcp")
    set_kind("binary")
    add_files("dev_mcp/*.cpp")
    add_deps("liminal-core")
    if is_plat("mingw") or is_plat("windows") then
        after_build(function (target)
            os.cp(path.join(os.scriptdir(), "dev_mcp/windows_pty.py"), target:targetdir())
        end)
    elseif is_plat("linux") then
        add_syslinks("util")
    end

includes("tests/xmake.lua")
