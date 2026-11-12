# Boost.Process v2 requires a compiled library in newer Boost releases.
if(Boost_VERSION VERSION_GREATER_EQUAL 1.87 AND NOT TARGET Boost::process)
  message(FATAL_ERROR "Install Boost.Process development libraries for this Boost version")
endif()
# SPDX-License-Identifier: GPL-3.0-or-later
# Included at the end of the pinned DwarFS project by project-hook.cmake.
add_executable(dwarfsctl-reader "${CMAKE_CURRENT_LIST_DIR}/main.cpp")
target_compile_features(dwarfsctl-reader PRIVATE cxx_std_23)
target_link_libraries(dwarfsctl-reader PRIVATE dwarfs_tool dwarfs_reader dwarfs_extractor mkdwarfs_main dwarfsck_main dwarfsextract_main dwarfs_main ${DWARFS_FMT_LIB})
target_compile_options(dwarfsctl-reader PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS dwarfsctl-reader RUNTIME DESTINATION libexec COMPONENT dwarfsctl-reader)
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION share/licenses/dwarfsctl/dwarfs
        COMPONENT dwarfsctl-reader)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/"
        DESTINATION share/licenses/dwarfsctl/dwarfs COMPONENT dwarfsctl-reader)

set_target_properties(dwarfsctl-reader PROPERTIES OUTPUT_NAME dwarfsctl-engine)
include("${CMAKE_CURRENT_LIST_DIR}/../cli/CMakeLists.txt")
