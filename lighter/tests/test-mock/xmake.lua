add_requires("ngcpp-proxy")

target("test-mock")
    set_kind("binary")
    set_group("test")
    add_files("main.cpp")
    add_deps("lighter")
    add_packages("ngcpp-proxy")
    add_tests("default")
