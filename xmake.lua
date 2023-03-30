set_project("chatgpt-html")
set_version("0.0.1", {build = "%Y%m%d%H%M"})
set_xmakever("2.7.7")

add_repositories("my_private_repo https://github.com/fantasy-peak/xmake-repo.git")
add_requires("drogon v1.8.4", "fmt", "yaml_cpp_struct v1.0.0", "nlohmann_json", "spdlog")

set_languages("c++23")
set_policy("check.auto_ignore_flags", false)
add_cxflags("-O2 -Wall -Wextra -Werror -pedantic-errors -Wno-missing-field-initializers -Wno-ignored-qualifiers")

target("chatgpt-html")
    set_kind("binary")
    add_files("src/*.cpp")
    add_packages("drogon", "fmt", "yaml_cpp_struct", "nlohmann_json", "spdlog")
    add_syslinks("pthread")
target_end()