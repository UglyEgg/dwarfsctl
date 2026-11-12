# SPDX-FileCopyrightText: 2026 Richard Majewski
# SPDX-License-Identifier: GPL-3.0-or-later
#
# KIO worker for DwarFS images: makes a .dwarfs image browsable as a first-class
# location (dwarfs:/path/to/image.dwarfs/inner/path) without mounting it.
#
# All image access goes through the C++ dwarfsctl backend and its pinned engine.

cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" DWARFSCTL_KIO_WORKER_VERSION)
string(STRIP "${DWARFSCTL_KIO_WORKER_VERSION}" DWARFSCTL_KIO_WORKER_VERSION)

project(dwarfsctl_kio_worker VERSION "${DWARFSCTL_KIO_WORKER_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Test)

# Many distros (including Arch) ship no top-level KF6Config.cmake; use the
# per-framework configs, as the rest of kde/ does.
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)
find_package(KF6KIO REQUIRED)

# KIO finds the protocol in the plugin's embedded metadata and launches the
# worker out of process through the exported kdemain entry point in main.cpp.
set(WORKER_SOURCES
    src/main.cpp
    src/dwarfs_worker.cpp
    src/dwarfs_worker.h
    src/dwarfsctl_cat.cpp
    src/dwarfsctl_cat.h
    src/dwarfsctl_ls.cpp
    src/dwarfsctl_ls.h
    src/error_mapping.cpp
    src/error_mapping.h
    src/url_resolver.cpp
    src/url_resolver.h
)

# Generate plugin metadata at configure time so the embedded version stays
# single-source, matching kde/kf6-plugin.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dwarfs.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dwarfs.json"
    @ONLY
)

kcoreaddons_add_plugin(dwarfs
    INSTALL_NAMESPACE kf6/kio
    SOURCES ${WORKER_SOURCES}
)

target_compile_definitions(dwarfs PRIVATE
    DWARFSCTL_KIO_JSON="${CMAKE_CURRENT_BINARY_DIR}/dwarfs.json"
)

target_link_libraries(dwarfs PRIVATE
    Qt6::Core
    KF6::CoreAddons
    KF6::I18n
    KF6::KIOCore
)

# --- Tests -------------------------------------------------------------------
# Both suites avoid I/O where it does not change what is being tested, so they run
# on any machine -- including CI, which has no DwarFS tools:
#
#   tst_url_resolver  pure logic; no KIO, no process execution.
#   tst_dwarfsctl_ls  parses recorded backend envelopes; runs no process.
#
# A third suite that drives real images belongs here only behind the
# DWARFSCTL_INTEGRATION=1 gate, so it cannot pass silently where tools are absent.
# i18n: install translations if provided (gettext via KI18n), mirroring the plugin.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/po")
    if(COMMAND ki18n_install)
        ki18n_install(po)
    endif()
endif()

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(tst_url_resolver
        tests/tst_url_resolver.cpp
        src/url_resolver.cpp
        src/url_resolver.h
    )
    target_include_directories(tst_url_resolver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_url_resolver PRIVATE Qt6::Core Qt6::Test)

    add_executable(tst_dwarfsctl_ls
        tests/tst_dwarfsctl_ls.cpp
        src/dwarfsctl_ls.cpp
        src/dwarfsctl_ls.h
    )
    target_include_directories(tst_dwarfsctl_ls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_dwarfsctl_ls PRIVATE Qt6::Core Qt6::Test)

    # The F4 regression suite: a fake backend counts real invocations, proving a
    # browse is one listing plus cached lookups rather than a re-list per stat.
    add_executable(tst_listing_lookup
        tests/tst_listing_lookup.cpp
        src/dwarfsctl_ls.cpp
        src/dwarfsctl_ls.h
    )
    target_include_directories(tst_listing_lookup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_listing_lookup PRIVATE Qt6::Core Qt6::Test)

    # End-to-end suite: runs the real `dwarfsctl ls` against a real image. Gated on
    # DWARFSCTL_INTEGRATION=1 at runtime -- it prints a loud SKIP and passes when
    # the gate is off, so it never fails a machine without DwarFS, and never
    # silently pretends to have run.
    # The mapping uses i18n(), so this target links KF6::I18n as well as Qt.
    add_executable(tst_error_mapping
        tests/tst_error_mapping.cpp
        src/error_mapping.cpp
        src/error_mapping.h
    )
    target_include_directories(tst_error_mapping PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # KIOCore is needed for the KIO error codes, not just I18n for the messages.
    target_link_libraries(tst_error_mapping PRIVATE Qt6::Core Qt6::Test KF6::I18n KF6::KIOCore)

    add_executable(tst_dwarfsctl_cat
        tests/tst_dwarfsctl_cat.cpp
        src/dwarfsctl_cat.cpp
        src/dwarfsctl_cat.h
    )
    target_include_directories(tst_dwarfsctl_cat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_dwarfsctl_cat PRIVATE Qt6::Core Qt6::Test)

    add_executable(tst_image_reader_integration
        tests/tst_image_reader_integration.cpp
        src/dwarfsctl_cat.cpp
        src/dwarfsctl_cat.h
        src/dwarfsctl_ls.cpp
        src/dwarfsctl_ls.h
    )
    target_include_directories(tst_image_reader_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_image_reader_integration PRIVATE Qt6::Core Qt6::Test)

    add_executable(tst_protocol_discovery tests/tst_protocol_discovery.cpp)
    target_link_libraries(tst_protocol_discovery PRIVATE Qt6::Core Qt6::Test KF6::KIOCore)

    enable_testing()
    add_test(NAME tst_url_resolver COMMAND tst_url_resolver)
    add_test(NAME tst_dwarfsctl_ls COMMAND tst_dwarfsctl_ls)
    add_test(NAME tst_dwarfsctl_cat COMMAND tst_dwarfsctl_cat)
    add_test(NAME tst_error_mapping COMMAND tst_error_mapping)
    add_test(NAME tst_listing_lookup COMMAND tst_listing_lookup)
    add_test(NAME tst_image_reader_integration COMMAND tst_image_reader_integration)
    add_test(NAME tst_protocol_discovery COMMAND tst_protocol_discovery)
    set_tests_properties(tst_protocol_discovery PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()
