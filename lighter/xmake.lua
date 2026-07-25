add_requires("libuv", {system = true})
add_requires("fmt", {system = true})
add_requires("libcurl", {system = true})
add_requires("glaze")

target("lighter")
    set_kind("static")
    add_files("async/**/*.cpp", "encoding/*.cpp", "http/*.cpp", "utils/*.cpp")
    add_packages("libuv", "fmt")
    add_packages("libcurl", {public = true})
    -- codec/json is header-only over glaze, so consumers need its headers too
    add_packages("glaze", {public = true})
    if is_os("windows") then
        add_syslinks("psapi", "user32", "advapi32", "iphlpapi", "userenv", "ws2_32", "dbghelp", "ole32", "shell32", {public = true})
    end

includes("tests/xmake.lua")
