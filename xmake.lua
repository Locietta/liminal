set_xmakever("3.0.0")
set_project("liminal")

-- use releasedbg for development builds, ship with release
add_rules("mode.release", "mode.releasedbg")

set_languages("cxxlatest")

local contract_semantic = "enforce"
if is_mode("release") then
    contract_semantic = "ignore"
end

-- Enable reflection and contracts on gcc 16+
add_cxxflags(
    "-freflection",
    "-fcontracts",
    "-fcontract-evaluation-semantic=" .. contract_semantic,
    {tools = {"gcc", "gxx"}, force = true})

-- The toolchain and the C dependencies both come from the Pixi environment, so
-- every xmake invocation has to happen under `pixi run`. The script scope has no
-- error primitives, hence the option below rather than a plain check here.
local conda_prefix = os.getenv("CONDA_PREFIX") or ""

option("__pixi_environment")
    set_showmenu(false)
    on_check(function (option)
        if not os.getenv("CONDA_PREFIX") then
            raise("this project builds inside its Pixi environment: use `pixi run build`, or `pixi run xmake ...`")
        end
        option:enable(true)
    end)
option_end()

if is_host("windows") then
    -- Force GCC until static reflection is available on the native Windows
    -- toolchains. That means Xmake's `mingw` platform rather than `windows`:
    -- the latter assumes MSVC library naming (foo.lib), so link detection fails
    -- for any C++ package Xmake builds from source (libfoo.a). This is only a
    -- default -- `xmake f -p windows` still overrides it.
    set_defaultplat("mingw")

    -- conda-forge lays the mingw sysroot out under <prefix>/Library.
    local mingw_root = path.join(conda_prefix, "Library")
    set_toolchains("mingw", {mingw = mingw_root})
    set_config("mingw", mingw_root)
end

if is_os("windows") then
    add_defines("_CRT_SECURE_NO_WARNINGS")
    add_defines("WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE", "NOMINMAX", "_WINDOWS")
    -- set_policy("build.optimization.lto", true)
else
    -- The shared libraries resolved out of the Pixi environment are not on the
    -- system loader path, so record it in the binary the way conda's own
    -- activation scripts do. Windows has no rpath; it finds the DLLs through
    -- the PATH entry Pixi activation adds.
    add_rpathdirs(path.join(conda_prefix, "lib"))
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
