set_xmakever("3.0.0")
set_project("liminal")

add_rules("mode.release", "mode.debug", "mode.releasedbg")

set_languages("cxxlatest")

if is_os("windows") then
    set_toolchains("mingw") -- force to use gcc on windows, as msvc and clang don't support reflection yet

    -- detect mingw from conda environment, if we are in one
    if os.getenv("CONDA_PREFIX") then
        local mingw_path = path.join(os.getenv("CONDA_PREFIX"), "Library", "ucrt64")
        if os.isdir(mingw_path) then
            set_toolchains("mingw", {mingw = mingw_path})
        end
    end

    add_defines("_CRT_SECURE_NO_WARNINGS")
    add_defines("WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE", "NOMINMAX", "_WINDOWS")
    -- set_policy("build.optimization.lto", true)
-- elseif is_os("linux") then
--     -- gcc has an ICE on std::source_location::current() when used in coroutines, so we use clang instead
--     set_toolchains("clang")
end

add_repositories("loia-pinned xmake", {rootdir = os.scriptdir()})
add_moduledirs("xmake/modules")

includes("xmake/rules/*.lua")

add_includedirs(".")

includes("*/xmake.lua")
