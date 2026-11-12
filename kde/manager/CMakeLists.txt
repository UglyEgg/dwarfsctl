# SPDX-FileCopyrightText: 2026 Richard Majewski
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_LIST_DIR}/../kf6-plugin/VERSION" DWARFSCTL_MANAGER_VERSION)
string(STRIP "${DWARFSCTL_MANAGER_VERSION}" DWARFSCTL_MANAGER_VERSION)
project(dwarfsctl_manager VERSION "${DWARFSCTL_MANAGER_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets DBus)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(KF6JobWidgets REQUIRED)
find_package(KF6Notifications REQUIRED)
find_package(KF6WidgetsAddons REQUIRED)

set(PLUGIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../kf6-plugin/src")
set(TRAY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tray/src")

# These are the same source components consumed by Dolphin and the tray. Keep
# this an internal static target: it shares implementation without creating a
# public library ABI that the project would have to support independently.
add_library(dwarfsctl-manager-ui STATIC
    src/managerwindow.cpp
    src/managerwindow.h
    src/visualizationwidgets.cpp
    src/visualizationwidgets.h
    ${PLUGIN_SRC}/bannerwidget.cpp
    ${PLUGIN_SRC}/builddialog.cpp
    ${PLUGIN_SRC}/buildfeedback.cpp
    ${PLUGIN_SRC}/checksumdialog.cpp
    ${PLUGIN_SRC}/detailsdialog.cpp
    ${PLUGIN_SRC}/dwarfsctljob.cpp
    ${PLUGIN_SRC}/dwarfsctljson.cpp
    ${PLUGIN_SRC}/dwarfsctlmodel.cpp
    ${PLUGIN_SRC}/dwarfsctlrunner.cpp
    ${PLUGIN_SRC}/envelopetext.cpp
    ${PLUGIN_SRC}/integritycheckdialog.cpp
    ${PLUGIN_SRC}/logging.cpp
    ${PLUGIN_SRC}/mountscache.cpp
    ${PLUGIN_SRC}/notifications.cpp
    ${PLUGIN_SRC}/profileeditordialog.cpp
    ${PLUGIN_SRC}/recommendation.cpp
    ${PLUGIN_SRC}/trayagentjob.cpp
    ${PLUGIN_SRC}/warnings.cpp
    ${TRAY_SRC}/settingsdialog.cpp
)
set_target_properties(dwarfsctl-manager-ui PROPERTIES AUTOMOC ON)
target_include_directories(dwarfsctl-manager-ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${PLUGIN_SRC}
    ${TRAY_SRC}
)
target_compile_definitions(dwarfsctl-manager-ui PRIVATE QT_NO_KEYWORDS)
target_link_libraries(dwarfsctl-manager-ui PUBLIC
    Qt6::Core Qt6::Widgets Qt6::DBus
    KF6::CoreAddons KF6::I18n KF6::JobWidgets KF6::Notifications
    KF6::KIOCore KF6::KIOWidgets KF6::KIOFileWidgets KF6::WidgetsAddons
)

qt_add_executable(dwarfsctl-manager src/main.cpp)
target_link_libraries(dwarfsctl-manager PRIVATE dwarfsctl-manager-ui)
target_compile_definitions(dwarfsctl-manager PRIVATE
    DWARFSCTL_MANAGER_VERSION="${DWARFSCTL_MANAGER_VERSION}")

install(TARGETS dwarfsctl-manager RUNTIME DESTINATION ${KDE_INSTALL_BINDIR})
install(FILES org.quest.entropy.dwarfsctl.Manager.desktop
    DESTINATION ${KDE_INSTALL_APPDIR})

option(BUILD_TESTING "Build the test suite" ON)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    add_executable(tst_manager_window tests/tst_manager_window.cpp)
    target_link_libraries(tst_manager_window PRIVATE dwarfsctl-manager-ui Qt6::Test)
    add_test(NAME tst_manager_window COMMAND tst_manager_window)
    set(MANAGER_TEST_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/test-config")
    file(MAKE_DIRECTORY "${MANAGER_TEST_CONFIG}")
    set_tests_properties(tst_manager_window PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${MANAGER_TEST_CONFIG}"
        TIMEOUT 30)
endif()
