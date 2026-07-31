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
    set_config("mingw", ucrt64_root)  -- for those who use git bash on windows...

    add_defines("_CRT_SECURE_NO_WARNINGS")
    add_defines("WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE", "NOMINMAX", "_WINDOWS")
    -- set_policy("build.optimization.lto", true)

    option("__msys2_package_manager")
        set_showmenu(false)
        on_check(function (option)
            import("package.manager.msys2.register")()
            option:enable(true)
        end)
    option_end()

end

-- Binaries built with the ucrt64 toolchain need its DLLs (gcc runtime, libuv,
-- fmt, curl, ...) at load time. `xmake run/test` injects the toolchain runenv,
-- but anything launching the exe directly (integration tests, a plain shell)
-- would need %MSYS2%\ucrt64\bin on PATH. Instead, walk the import table with
-- objdump and copy the ucrt64 DLLs next to the binary.
rule("mingw.bundle-dlls")
    after_build(function (target)
        if not target:is_plat("windows") then
            return
        end
        local bindir = path.join(os.getenv("MSYS2"), "ucrt64", "bin")
        local objdump = path.join(bindir, "objdump.exe")
        local seen = {}
        local function bundle(file)
            for name in os.iorunv(objdump, {"-p", file}):gmatch("DLL Name: (%S+)") do
                if not seen[name] then
                    seen[name] = true
                    -- System DLLs (kernel32, ...) don't live in ucrt64/bin.
                    local src = path.join(bindir, name)
                    if os.isfile(src) then
                        local dst = path.join(target:targetdir(), name)
                        if not os.isfile(dst) or os.mtime(src) > os.mtime(dst) then
                            os.cp(src, dst)
                        end
                        bundle(src)
                    end
                end
            end
        end
        bundle(target:targetfile())
    end)
rule_end()

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
