cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" DWARFSCTL_TRAY_VERSION)
string(STRIP "${DWARFSCTL_TRAY_VERSION}" DWARFSCTL_TRAY_VERSION)

project(dwarfsctl_tray VERSION "${DWARFSCTL_TRAY_VERSION}" LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets DBus Svg)

# Arch (and many distros) do not ship a top-level KF6Config.cmake.
# Prefer the per-framework package configs.
find_package(KF6StatusNotifierItem REQUIRED)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(KF6JobWidgets REQUIRED)
find_package(KF6IconThemes 6.3 REQUIRED)
find_package(KF6WidgetsAddons REQUIRED)

# Keep the About dialog's plugin version string aligned with the Dolphin plugin.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../kf6-plugin/VERSION" DWARFSCTL_DOLPHIN_VERSION)
string(STRIP "${DWARFSCTL_DOLPHIN_VERSION}" DWARFSCTL_DOLPHIN_VERSION)

qt_standard_project_setup()

# Ensure resources are compiled even on older CMake/Qt macro combinations.
qt_add_resources(TRAY_RESOURCES tray.qrc)

qt_add_executable(dwarfsctl-tray
  src/main.cpp
  src/directmountoperation.cpp
  src/directmountoperation.h
  ../common/operationstatus.cpp
  src/aboutdialog.cpp
  src/aboutdialog.h
  ../common/dwarfsctlrunner.cpp
  ../common/dwarfsctlrunner.h
  src/mount_executor.cpp
  src/mount_executor.h
  src/mount_liveness.cpp
  src/mount_liveness.h
  src/settingsdialog.cpp
  src/settingsdialog.h
  ../kf6-plugin/src/bannerwidget.cpp
  ../kf6-plugin/src/bannerwidget.h
  ../kf6-plugin/src/dwarfsctljson.cpp
  ../kf6-plugin/src/dwarfsctljson.h
  ../kf6-plugin/src/logging.cpp
  ../kf6-plugin/src/logging.h
  ../kf6-plugin/src/profileeditordialog.cpp
  ../kf6-plugin/src/profileeditordialog.h
  src/traysettings.h
  src/placesmanager.cpp
  src/placesmanager.h
  src/symboliciconrenderer.cpp
  src/symboliciconrenderer.h
  ${TRAY_RESOURCES}
)

target_compile_definitions(dwarfsctl-tray PRIVATE DWARFSCTL_PLUGIN_VERSION="${DWARFSCTL_DOLPHIN_VERSION}")

# Expose tray version and D-Bus API major to the About dialog.
target_compile_definitions(dwarfsctl-tray PRIVATE DWARFSCTL_TRAY_VERSION="${DWARFSCTL_TRAY_VERSION}")
target_compile_definitions(dwarfsctl-tray PRIVATE DWARFSCTL_DBUS_API_MAJOR=1)

target_link_libraries(dwarfsctl-tray PRIVATE
  Qt6::Core
  Qt6::Widgets
  Qt6::DBus
  Qt6::Svg
  KF6::StatusNotifierItem
  KF6::CoreAddons
  KF6::I18n
  KF6::IconThemes
  KF6::KIOWidgets
  KF6::KIOFileWidgets
  KF6::WidgetsAddons
)

target_include_directories(dwarfsctl-tray PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
  ${CMAKE_CURRENT_SOURCE_DIR}/../kf6-plugin/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

install(TARGETS dwarfsctl-tray RUNTIME DESTINATION bin)

# Tray StatusNotifier icons. Install as theme icons so Plasma can render them
# at the correct panel size (no app-side rasterization).
set(DWARFSCTL_ICON_DIR "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/status")

install(FILES resources/helmet-unmounted.svg
  DESTINATION "${DWARFSCTL_ICON_DIR}"
  # Use -symbolic so Plasma recolors to match panel foreground.
  RENAME "dwarfsctl-tray-symbolic.svg"
)
install(FILES resources/helmet-mounted.svg
  DESTINATION "${DWARFSCTL_ICON_DIR}"
  RENAME "dwarfsctl-tray-mounted-symbolic.svg"
)

install(FILES resources/dwarfsctl-volume.svg
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/devices"
)

# The color application identity shown by launchers, task switchers and dialogs.
# This remains separate from the mounted-volume icon used in Dolphin Places.
install(FILES resources/dwarfsctl-app.svg
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
  RENAME "dwarfsctl.svg"
)

# --- Tests -------------------------------------------------------------------
# The rule the mount fix rests on is pure logic, so it is tested without a FUSE
# mount and runs on any machine -- including CI, which has no /dev/fuse. The
# end-to-end reproduction of a dead FUSE mount is gated at runtime on
# DWARFSCTL_INTEGRATION=1 so it never fails a machine without the tools, and
# never silently pretends to have run.
option(BUILD_TESTING "Build the test suite" ON)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(tst_mount_liveness
        tests/tst_mount_liveness.cpp
        src/mount_liveness.cpp
        src/mount_liveness.h
    )
    target_include_directories(tst_mount_liveness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_mount_liveness PRIVATE Qt6::Core Qt6::Test)

    enable_testing()
    add_test(NAME tst_mount_liveness COMMAND tst_mount_liveness)

    add_executable(tst_about_dialog
        tests/tst_about_dialog.cpp
        src/aboutdialog.cpp
        src/aboutdialog.h
        ../common/dwarfsctlrunner.cpp
        ${TRAY_RESOURCES}
    )
    target_include_directories(tst_about_dialog PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
    )
    target_compile_definitions(tst_about_dialog PRIVATE
        DWARFSCTL_TRAY_VERSION="${DWARFSCTL_TRAY_VERSION}"
        DWARFSCTL_PLUGIN_VERSION="${DWARFSCTL_DOLPHIN_VERSION}"
        DWARFSCTL_DBUS_API_MAJOR=1
        DWARFSCTL_TEST_CHANGELOG="${CMAKE_CURRENT_SOURCE_DIR}/../../CHANGELOG.md"
    )
    target_link_libraries(tst_about_dialog PRIVATE
        Qt6::Widgets Qt6::Test KF6::I18n KF6::WidgetsAddons
    )
    add_test(NAME tst_about_dialog COMMAND tst_about_dialog)
    set_tests_properties(tst_about_dialog PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;DWARFSCTL_BIN=/usr/bin/false"
        TIMEOUT 30
    )

    add_executable(tst_symbolic_icon
        tests/tst_symbolic_icon.cpp
        src/symboliciconrenderer.cpp
        src/symboliciconrenderer.h
    )
    target_compile_definitions(tst_symbolic_icon PRIVATE
        DWARFSCTL_TEST_ICON="${CMAKE_CURRENT_SOURCE_DIR}/resources/helmet-mounted.svg")
    target_include_directories(tst_symbolic_icon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst_symbolic_icon PRIVATE Qt6::Gui Qt6::Svg Qt6::Test)
    add_test(NAME tst_symbolic_icon COMMAND tst_symbolic_icon)
    set_tests_properties(tst_symbolic_icon PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_profile_editor
        tests/tst_profile_editor.cpp
        ../common/dwarfsctlrunner.cpp
        ../kf6-plugin/src/bannerwidget.cpp
        ../kf6-plugin/src/dwarfsctljson.cpp
        ../kf6-plugin/src/profileeditordialog.cpp
    )
    target_include_directories(tst_profile_editor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
        ${CMAKE_CURRENT_SOURCE_DIR}/../kf6-plugin/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(tst_profile_editor PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test KF6::I18n KF6::WidgetsAddons
    )
    add_test(NAME tst_profile_editor COMMAND tst_profile_editor)
    set_tests_properties(tst_profile_editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_settings_dialog
        tests/tst_settings_dialog.cpp
        src/settingsdialog.cpp
        ../common/dwarfsctlrunner.cpp
        ../kf6-plugin/src/bannerwidget.cpp
        ../kf6-plugin/src/dwarfsctljson.cpp
        ../kf6-plugin/src/profileeditordialog.cpp
    )
    target_include_directories(tst_settings_dialog PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
        ${CMAKE_CURRENT_SOURCE_DIR}/../kf6-plugin/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(tst_settings_dialog PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test KF6::I18n KF6::WidgetsAddons
    )
    add_test(NAME tst_settings_dialog COMMAND tst_settings_dialog)
    set_tests_properties(tst_settings_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(BUILD_TESTING)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(NOT DBUS_RUN_SESSION)
        message(FATAL_ERROR "dbus-run-session is required for the runtime contract test")
    endif()
    add_executable(tst_dbus_runtime tests/tst_dbus_runtime.cpp)
    target_link_libraries(tst_dbus_runtime PRIVATE Qt6::Core Qt6::DBus Qt6::Test)
    add_dependencies(tst_dbus_runtime dwarfsctl-tray)
    add_test(NAME tst_dbus_runtime
        COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_dbus_runtime> $<TARGET_FILE:dwarfsctl-tray>)
    set_tests_properties(tst_dbus_runtime PROPERTIES TIMEOUT 45)
endif()

if(BUILD_TESTING)
    add_executable(tst_direct_mount tests/tst_direct_mount.cpp
        src/directmountoperation.cpp src/directmountoperation.h ../common/operationstatus.cpp)
    target_include_directories(tst_direct_mount PRIVATE src ../common)
    target_link_libraries(tst_direct_mount PRIVATE Qt6::Core Qt6::Widgets Qt6::Test KF6::I18n)
    add_test(NAME tst_direct_mount COMMAND tst_direct_mount)
    set_tests_properties(tst_direct_mount PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
endif()

if(BUILD_TESTING)
    add_executable(tst_imagehistory tests/tst_imagehistory.cpp)
    target_include_directories(tst_imagehistory PRIVATE ../common)
    target_link_libraries(tst_imagehistory PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME tst_imagehistory COMMAND tst_imagehistory)
    set_tests_properties(tst_imagehistory PROPERTIES TIMEOUT 30)
    add_executable(tst_image_menu tests/tst_image_menu.cpp)
    target_include_directories(tst_image_menu PRIVATE src ../common)
    target_link_libraries(tst_image_menu PRIVATE Qt6::Core Qt6::Test KF6::I18n)
    add_test(NAME tst_image_menu COMMAND tst_image_menu)
endif()

if(BUILD_TESTING)
    add_executable(tst_mount_executor tests/tst_mount_executor.cpp
        src/mount_executor.cpp src/mount_liveness.cpp)
    target_include_directories(tst_mount_executor PRIVATE src ../common)
    target_link_libraries(tst_mount_executor PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME tst_mount_executor COMMAND tst_mount_executor)
    set_tests_properties(tst_mount_executor PROPERTIES TIMEOUT 30)
endif()

# GUI queries share the same bounded process ownership as image operations.
foreach(gui_target dwarfsctl-tray tst_about_dialog tst_profile_editor tst_settings_dialog)
    if(TARGET ${gui_target})
        target_sources(${gui_target} PRIVATE
            ../kf6-plugin/src/dwarfsctljob.cpp
            ../kf6-plugin/src/dwarfsctljob.h
            ../kf6-plugin/src/logging.cpp)
        target_include_directories(${gui_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../kf6-plugin/src)
        target_link_libraries(${gui_target} PRIVATE Qt6::DBus KF6::CoreAddons KF6::JobWidgets KF6::KIOWidgets)
    endif()
endforeach()
if(TARGET tst_about_dialog)
    target_sources(tst_about_dialog PRIVATE ../kf6-plugin/src/dwarfsctljson.cpp)
endif()
