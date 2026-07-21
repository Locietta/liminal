set_xmakever("3.0.0")
set_project("liminal")

add_rules("mode.release", "mode.debug", "mode.releasedbg")

set_languages("cxxlatest")

if is_os("windows") then
    local msys2_root = os.getenv("MSYS2")
    if not msys2_root then
        raise("Please set %MSYS2% to the root of your MSYS2 installation.")
    end
    local ucrt64_root = path.join(msys2_root, "ucrt64")
    if not os.isdir(ucrt64_root) then
        raise("MSYS2 does not contain the ucrt64 environment: " .. ucrt64_root)
    end

    -- Force GCC until static reflection is available on the native Windows toolchains.
    set_toolchains("mingw", {mingw = ucrt64_root})

    add_defines("_CRT_SECURE_NO_WARNINGS")
    add_defines("WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE", "NOMINMAX", "_WINDOWS")
    -- set_policy("build.optimization.lto", true)
-- elseif is_os("linux") then
--     -- gcc has an ICE on std::source_location::current() when used in coroutines, so we use clang instead
--     set_toolchains("clang")
end

add_repositories("loia-pinned xmake", {rootdir = os.scriptdir()})
add_moduledirs("xmake/modules")

option("__pixi_package_manager")
    set_showmenu(false)
    on_check(function (option)
        import("package.manager.pixi.register")()
        option:enable(true)
    end)
option_end()

includes("xmake/rules/*.lua")

add_includedirs(".")

includes("*/xmake.lua")
