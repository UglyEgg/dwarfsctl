# SPDX-License-Identifier: GPL-3.0-or-later
# DwarFS requires itself to be CMAKE_SOURCE_DIR. Defer our target until its
# targets exist; no installed unstable ABI is needed.
find_path(DWARFSCTL_UTF8_INCLUDE_DIR utf8.h PATH_SUFFIXES utf8cpp REQUIRED)
include_directories(SYSTEM "${DWARFSCTL_UTF8_INCLUDE_DIR}")
cmake_language(DEFER CALL include "${DWARFSCTL_READER_SOURCE}/CMakeLists.txt")

# Enforce stored-name containment in the extractor shared by every entry point.
# A strict reverse check makes repeated configure safe without accepting drift.
find_program(DWARFSCTL_PATCH_PROGRAM patch REQUIRED)
set(DWARFSCTL_EXTRACTION_PATCH "${DWARFSCTL_READER_SOURCE}/../../packaging/patches/dwarfs-0.15.10-extraction-names.patch")
execute_process(COMMAND "${DWARFSCTL_PATCH_PROGRAM}" --dry-run --silent --fuzz=0 --forward -p1
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" INPUT_FILE "${DWARFSCTL_EXTRACTION_PATCH}"
  RESULT_VARIABLE DWARFSCTL_EXTRACTION_PATCH_STATUS OUTPUT_QUIET ERROR_QUIET)
if(DWARFSCTL_EXTRACTION_PATCH_STATUS EQUAL 0)
  execute_process(COMMAND "${DWARFSCTL_PATCH_PROGRAM}" --batch --fuzz=0 --forward -p1
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" INPUT_FILE "${DWARFSCTL_EXTRACTION_PATCH}"
    RESULT_VARIABLE DWARFSCTL_EXTRACTION_PATCH_STATUS)
  if(NOT DWARFSCTL_EXTRACTION_PATCH_STATUS EQUAL 0)
    message(FATAL_ERROR "DwarFS extraction containment patch could not be applied")
  endif()
else()
  execute_process(COMMAND "${DWARFSCTL_PATCH_PROGRAM}" --dry-run --silent --fuzz=0 --reverse -p1
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" INPUT_FILE "${DWARFSCTL_EXTRACTION_PATCH}"
    RESULT_VARIABLE DWARFSCTL_EXTRACTION_PATCH_STATUS OUTPUT_QUIET ERROR_QUIET)
  if(NOT DWARFSCTL_EXTRACTION_PATCH_STATUS EQUAL 0)
    message(FATAL_ERROR "DwarFS extraction containment patch does not match the source tree")
  endif()
endif()
