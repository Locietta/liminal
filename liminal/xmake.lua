target("liminal")
    set_kind("binary")
    add_files("**/*.cpp", "*.cpp")
    add_deps("lighter")
