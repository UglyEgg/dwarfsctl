# SPDX-License-Identifier: GPL-3.0-or-later
# DwarFS requires itself to be CMAKE_SOURCE_DIR. Defer our target until its
# targets exist; no upstream source changes or installed unstable ABI needed.
find_path(DWARFSCTL_UTF8_INCLUDE_DIR utf8.h PATH_SUFFIXES utf8cpp REQUIRED)
include_directories(SYSTEM "${DWARFSCTL_UTF8_INCLUDE_DIR}")
cmake_language(DEFER CALL include "${DWARFSCTL_READER_SOURCE}/CMakeLists.txt")
