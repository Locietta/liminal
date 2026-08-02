add_requires("pixi::libcurl", {alias = "libcurl"})
add_requires("pixi::libiconv", {alias = "libiconv"})
add_requires("pixi::libuv", {alias = "libuv"})
add_requires("glaze")

target("lighter")
    set_kind("static")
    add_files("async/**/*.cpp", "encoding/*.cpp", "http/*.cpp", "utils/*.cpp")
    add_packages("libuv")
    add_packages("libiconv")
    add_syslinks("stdc++exp", {public = true})
    add_packages("libcurl", {public = true})
    -- codec/json is header-only over glaze, so consumers need its headers too
    add_packages("glaze", {public = true})
    if is_os("windows") then
        add_syslinks("psapi", "user32", "advapi32", "iphlpapi", "userenv", "ws2_32", "dbghelp", "ole32", "shell32", {public = true})
    elseif is_os("linux") then
        add_syslinks("pthread", {public = true})
    end

includes("tests/xmake.lua")
