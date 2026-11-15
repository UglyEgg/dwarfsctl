# SPDX-FileCopyrightText: 2026 Richard Majewski
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" DWARFSCTL_DOLPHIN_VERSION)
string(STRIP "${DWARFSCTL_DOLPHIN_VERSION}" DWARFSCTL_DOLPHIN_VERSION)

project(dwarfsctl_dolphin_plugin VERSION "${DWARFSCTL_DOLPHIN_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets DBus)

# Many distros (including Arch) do not ship a top-level KF6Config.cmake.
# Prefer the per-framework package configs.
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(KF6JobWidgets REQUIRED)
find_package(KF6Notifications REQUIRED)
find_package(KF6WidgetsAddons REQUIRED)

include("${KF6CoreAddons_DIR}/KF6CoreAddonsMacros.cmake")
include("${KF6I18n_DIR}/KF6I18nMacros.cmake")

set(COMMON_SOURCES
    src/dwarfsctlrunner.cpp
    src/dwarfsctlrunner.h
    src/dwarfsctljob.cpp
    src/dwarfsctljob.h
    src/envelopetext.cpp
    src/envelopetext.h
    src/trayagentjob.cpp
    src/trayagentjob.h
    src/mountscache.cpp
    src/mountscache.h
    src/dwarfsctljson.cpp
    src/dwarfsctljson.h
    src/dwarfsctlmodel.cpp
    src/dwarfsctlmodel.h
    src/warnings.cpp
    src/warnings.h
    src/logging.cpp
    src/logging.h
    src/notifications.cpp
    src/notifications.h
    src/detailsdialog.cpp
    src/detailsdialog.h
)

set(FILEITEM_PLUGIN_SOURCES
    src/recommendation.cpp
    src/recommendation.h
    src/bannerwidget.cpp
    src/bannerwidget.h
    src/builddialog.cpp
    src/builddialog.h
    src/integritycheckdialog.cpp
    src/integritycheckdialog.h
    src/checksumdialog.cpp
    src/checksumdialog.h
    src/dwarfsfileitemactionplugin.cpp
    src/dwarfsfileitemactionplugin.h
)

set(PROPERTIES_PLUGIN_SOURCES
    src/statusadvanceddialog.cpp
    src/statusadvanceddialog.h
    src/dwarfspropertiesplugin.cpp
    src/dwarfspropertiesplugin.h
)



# Generate plugin metadata at configure time so versioning stays single-source.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/metadata.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/metadata.json"
    @ONLY
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dwarfspropertiesplugin.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dwarfspropertiesplugin.json"
    @ONLY
)

kcoreaddons_add_plugin(dwarfsfileitemactionplugin
    SOURCES ${COMMON_SOURCES} ${FILEITEM_PLUGIN_SOURCES}
    INSTALL_NAMESPACE "kf6/kfileitemaction"
)

kcoreaddons_add_plugin(dwarfspropertiesplugin
    SOURCES ${COMMON_SOURCES} ${PROPERTIES_PLUGIN_SOURCES}
    INSTALL_NAMESPACE "kf6/propertiesdialog"
)

target_link_libraries(dwarfsfileitemactionplugin
    Qt6::Core
    Qt6::Widgets
    Qt6::DBus
    KF6::CoreAddons
    KF6::I18n
    KF6::JobWidgets
    KF6::Notifications
    KF6::KIOWidgets
    KF6::KIOFileWidgets
    KF6::WidgetsAddons
)

# kf6-plugin shares kde/common with the tray and thumbnailer (uiconfig.h owns
# the `[ui]` read, so no front end parses the config file itself).
target_include_directories(dwarfsfileitemactionplugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_include_directories(dwarfspropertiesplugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_link_libraries(dwarfspropertiesplugin
    Qt6::Core
    Qt6::Widgets
    Qt6::DBus
    KF6::CoreAddons
    KF6::I18n
    KF6::JobWidgets
    KF6::Notifications
    KF6::KIOWidgets
    KF6::KIOFileWidgets
    KF6::WidgetsAddons
)

target_compile_definitions(dwarfsfileitemactionplugin PRIVATE QT_NO_KEYWORDS)
target_compile_definitions(dwarfspropertiesplugin PRIVATE QT_NO_KEYWORDS)

# Provide plugin version string for About dialog.
target_compile_definitions(dwarfsfileitemactionplugin PRIVATE DWARFSCTL_PLUGIN_VERSION=\"${DWARFSCTL_DOLPHIN_VERSION}\")
target_compile_definitions(dwarfspropertiesplugin PRIVATE DWARFSCTL_PLUGIN_VERSION=\"${DWARFSCTL_DOLPHIN_VERSION}\")

# Embed generated JSON metadata (includes the version from VERSION).
target_compile_definitions(
    dwarfsfileitemactionplugin
    PRIVATE DWARFSCTL_DOLPHIN_METADATA_FILE=\"${CMAKE_CURRENT_BINARY_DIR}/metadata.json\"
)

target_compile_definitions(
    dwarfspropertiesplugin
    PRIVATE DWARFSCTL_DOLPHIN_METADATA_FILE=\"${CMAKE_CURRENT_BINARY_DIR}/dwarfspropertiesplugin.json\"
)

# i18n: install translations if provided (gettext via KI18n).
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/po")
  if (COMMAND ki18n_install)
    ki18n_install(po)
  endif()
endif()

# --- Tests -------------------------------------------------------------------
# The envelope-text helpers shared by the checksum and integrity dialogs are
# pure string handling, so the suite runs anywhere -- including CI, which has no
# DwarFS tools and no /dev/fuse. Enable tests explicitly: relying on the default
# would silently stop running this if the default ever changed.
option(BUILD_TESTING "Build the test suite" ON)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(tst_envelopetext
        tests/tst_envelopetext.cpp
        src/envelopetext.cpp
        src/envelopetext.h
    )
    target_include_directories(tst_envelopetext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_envelopetext PRIVATE Qt6::Core Qt6::Test)

    # The `[ui]` reader the front ends use instead of opening config.toml. The
    # parse rule is pure JSON handling, so it runs without a backend installed.
    add_executable(tst_uiconfig
        tests/tst_uiconfig.cpp
    )
    target_include_directories(tst_uiconfig PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
    )
    target_link_libraries(tst_uiconfig PRIVATE Qt6::Core Qt6::Test)

    enable_testing()
    add_test(NAME tst_envelopetext COMMAND tst_envelopetext)
    add_test(NAME tst_uiconfig COMMAND tst_uiconfig)

    add_executable(tst_statusmodel
        tests/tst_statusmodel.cpp
        src/dwarfsctlmodel.cpp
        src/dwarfsctlmodel.h
        src/dwarfsctljson.cpp
        src/dwarfsctljson.h
    )
    target_include_directories(tst_statusmodel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_statusmodel PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME tst_statusmodel COMMAND tst_statusmodel)
endif()

if(BUILD_TESTING)
    add_executable(tst_recommendation tests/tst_recommendation.cpp src/recommendation.cpp)
    target_include_directories(tst_recommendation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_recommendation PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME tst_recommendation COMMAND tst_recommendation)
endif()

if(BUILD_TESTING)
    add_executable(tst_buildrecommendation
        tests/tst_buildrecommendation.cpp
        src/builddialog.cpp src/bannerwidget.cpp
        src/recommendation.cpp src/dwarfsctljob.cpp src/dwarfsctlrunner.cpp
        src/dwarfsctljson.cpp src/warnings.cpp src/logging.cpp)
    target_include_directories(tst_buildrecommendation PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/../common)
    target_link_libraries(tst_buildrecommendation PRIVATE
        Qt6::Core Qt6::Widgets Qt6::DBus Qt6::Test
        KF6::CoreAddons KF6::I18n KF6::JobWidgets KF6::KIOWidgets KF6::WidgetsAddons)
    target_compile_definitions(tst_buildrecommendation PRIVATE QT_NO_KEYWORDS)
    add_test(NAME tst_buildrecommendation COMMAND tst_buildrecommendation)
    set_tests_properties(tst_buildrecommendation PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()

target_sources(dwarfsfileitemactionplugin PRIVATE src/buildfeedback.cpp src/buildfeedback.h)
if(BUILD_TESTING)
    add_executable(tst_buildfeedback tests/tst_buildfeedback.cpp src/buildfeedback.cpp)
    target_include_directories(tst_buildfeedback PRIVATE src)
    target_link_libraries(tst_buildfeedback PRIVATE Qt6::Widgets Qt6::Test KF6::I18n)
    add_test(NAME tst_buildfeedback COMMAND tst_buildfeedback)
    set_tests_properties(tst_buildfeedback PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(BUILD_TESTING)
    find_program(UPDATE_MIME_DATABASE_EXECUTABLE update-mime-database REQUIRED)
    set(TEST_XDG_DATA_HOME "${CMAKE_CURRENT_BINARY_DIR}/test-xdg-data")
    file(MAKE_DIRECTORY "${TEST_XDG_DATA_HOME}/mime/packages")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/../mime/packages/application-x-dwarfs.xml"
        "${TEST_XDG_DATA_HOME}/mime/packages/application-x-dwarfs.xml"
        COPYONLY
    )
    execute_process(
        COMMAND "${UPDATE_MIME_DATABASE_EXECUTABLE}" "${TEST_XDG_DATA_HOME}/mime"
        RESULT_VARIABLE TEST_MIME_DATABASE_RESULT
    )
    if(NOT TEST_MIME_DATABASE_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not prepare the DwarFS MIME database for the Properties discovery test")
    endif()

    add_executable(tst_propertiesdiscovery tests/tst_propertiesdiscovery.cpp)
    target_link_libraries(tst_propertiesdiscovery PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test
        KF6::KIOCore KF6::KIOWidgets)
    add_dependencies(tst_propertiesdiscovery dwarfspropertiesplugin)
    add_test(NAME tst_propertiesdiscovery COMMAND tst_propertiesdiscovery)
    set_tests_properties(tst_propertiesdiscovery PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${CMAKE_BINARY_DIR}/bin;XDG_DATA_HOME=${TEST_XDG_DATA_HOME}"
        TIMEOUT 30)
endif()

if(BUILD_TESTING)
    add_executable(tst_dwarfsctljob tests/tst_dwarfsctljob.cpp
        src/dwarfsctljob.cpp src/dwarfsctlrunner.cpp src/dwarfsctljson.cpp src/logging.cpp)
    target_include_directories(tst_dwarfsctljob PRIVATE src ../common)
    target_link_libraries(tst_dwarfsctljob PRIVATE Qt6::Core Qt6::Widgets Qt6::DBus
        Qt6::Test KF6::CoreAddons KF6::I18n KF6::JobWidgets KF6::KIOWidgets)
    target_compile_definitions(tst_dwarfsctljob PRIVATE QT_NO_KEYWORDS)
    add_test(NAME tst_dwarfsctljob COMMAND tst_dwarfsctljob)
    set_tests_properties(tst_dwarfsctljob PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
endif()
