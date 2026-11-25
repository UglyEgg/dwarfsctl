# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 6.4 REQUIRED COMPONENTS Core DBus)
find_package(tomlplusplus REQUIRED)
set(CTL_SOURCE "${CMAKE_CURRENT_LIST_DIR}")
file(READ "${CTL_SOURCE}/../../resources/mkdwarfs_schema_v0_15_10.json" CTL_SCHEMA)
file(READ "${CTL_SOURCE}/../../packaging/etc/dwarfs-dolphin/config.toml" CTL_PROFILES)
configure_file("${CTL_SOURCE}/resources.cpp.in" "${CMAKE_BINARY_DIR}/ctl-resources.cpp" @ONLY)
add_executable(dwarfsctl
  "${CTL_SOURCE}/main.cpp" "${CTL_SOURCE}/arguments.cpp" "${CTL_SOURCE}/core.cpp"
  "${CTL_SOURCE}/config.cpp" "${CTL_SOURCE}/operations.cpp" "${CTL_SOURCE}/recommend.cpp"
  "${CTL_SOURCE}/schema.cpp" "${CTL_SOURCE}/diagnostics.cpp" "${CMAKE_BINARY_DIR}/ctl-resources.cpp")
target_include_directories(dwarfsctl PRIVATE "${CTL_SOURCE}")
target_compile_features(dwarfsctl PRIVATE cxx_std_23)
target_compile_options(dwarfsctl PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dwarfsctl PRIVATE Qt6::Core Qt6::DBus tomlplusplus::tomlplusplus)
install(TARGETS dwarfsctl RUNTIME DESTINATION bin COMPONENT dwarfsctl-reader)

file(STRINGS "${CTL_SOURCE}/../../VERSION" CTL_VERSION LIMIT_COUNT 1)
target_compile_definitions(dwarfsctl PRIVATE CTL_VERSION="${CTL_VERSION}")
