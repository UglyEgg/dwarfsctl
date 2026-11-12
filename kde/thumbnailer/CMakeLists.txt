cmake_minimum_required(VERSION 3.16)
project(dwarfsctl_thumbnailer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_executable(dwarfsctl-thumbnailer src/main.cpp)
target_link_libraries(dwarfsctl-thumbnailer PRIVATE Qt6::Core Qt6::Gui)

target_include_directories(dwarfsctl-thumbnailer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

install(TARGETS dwarfsctl-thumbnailer RUNTIME DESTINATION bin)
install(FILES dwarfs-thumbnailer.desktop DESTINATION share/thumbnailers)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME thumbnail_render COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/thumbnail.py $<TARGET_FILE:dwarfsctl-thumbnailer>)
endif()
