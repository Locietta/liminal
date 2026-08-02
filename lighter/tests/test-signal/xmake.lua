target("test-signal")
    set_kind("binary")
    set_group("test")
    add_files("main.cpp")
    add_deps("lighter")
    if is_plat("linux") then
        add_syslinks("pthread")
    end
    add_tests("default")
