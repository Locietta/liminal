add_requires("ngcpp-proxy")

target("liminal")
    set_kind("binary")
    add_files("agent/*.cpp", "provider/*.cpp", "tools/*.cpp", "main.cpp")
    add_deps("lighter")
    add_packages("ngcpp-proxy")
    add_rules("mingw.bundle-dlls")
