add_requires("libuv", {system = true})
add_requires("fmt", {system = true})
add_requires("libcurl", {system = true})
add_requires("glaze")

-- glibc provides iconv_open/iconv/iconv_close in libc itself, so Linux needs
-- neither a package nor -liconv. Only the MinGW/UCRT64 build pulls in GNU
-- libiconv as a separate library.
if is_os("windows") then
    add_requires("msys2-ucrt64::libiconv", {alias = "libiconv"})
end

target("lighter")
    set_kind("static")
    add_files("async/**/*.cpp", "encoding/*.cpp", "http/*.cpp", "utils/*.cpp")
    add_packages("libuv", "fmt")
    on_config(function (target)
        if target:has_tool("cxx", "gcc") or target:has_tool("cxx", "gxx") then
            target:add("syslinks", "stdc++exp", {public = true})
        end
    end)
    if is_os("windows") then
        add_packages("libiconv")
    end
    add_packages("libcurl", {public = true})
    -- codec/json is header-only over glaze, so consumers need its headers too
    add_packages("glaze", {public = true})
    if is_os("windows") then
        add_syslinks("psapi", "user32", "advapi32", "iphlpapi", "userenv", "ws2_32", "dbghelp", "ole32", "shell32", {public = true})
    end

includes("tests/xmake.lua")
