includes("xmake/rules/*.lua")

add_requires("lighter 0.4.0", "lilac 0.1.0", "boost_uuid 1.91.0", "libunicode 0.9.2", "ngcpp-proxy", "sqlite3 >=3.51.3 <4")

if is_plat("mingw") then
    -- lighter's glaze and llhttp install through CMake, whose compiler check links
    -- a test program and so also needs the Pixi MinGW CRT search path (see the root xmake.lua).
    add_requireconfs("lighter.glaze", "lighter.llhttp", {configs = {
        ldflags = "-B" .. path.join(os.getenv("CONDA_PREFIX"), "Library", "x86_64-w64-mingw32", "sysroot", "usr", "lib") .. "/"
    }})
end

target("liminal-core")
    set_kind("static")
    add_rules("utils.bin2obj.cpp", {extensions = {".md"}})
    add_rules("utils.version.cpp")
    add_files("agent/*.cpp", "application/*.cpp", "context/*.cpp", "model/*.cpp", "provider/*.cpp", "provider/detail/*.cpp", "session/*.cpp", "tools/*.cpp", "tui/*.cpp")
    add_files("prompts/default-agent.md", "prompts/runtime-tools.md")
    add_packages("lighter", "lilac", {public = true})
    add_packages("boost_uuid", "sqlite3")
    add_packages("libunicode", "ngcpp-proxy", {public = true})

target("liminal")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("liminal-core")

includes("tests/xmake.lua")
