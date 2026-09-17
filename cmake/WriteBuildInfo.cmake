cmake_minimum_required(VERSION 3.24)

foreach(required SOURCE_DIR OUTPUT_FILE APP_VERSION BUILD_ARCH BUILD_CONFIG)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Build identity requires ${required}.")
    endif()
endforeach()
cmake_path(ABSOLUTE_PATH SOURCE_DIR NORMALIZE)
if(NOT APP_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Build identity requires a three-part numeric app version.")
endif()
set(version_parts "${CMAKE_MATCH_1};${CMAKE_MATCH_2};${CMAKE_MATCH_3}")
foreach(part IN LISTS version_parts)
    if(part GREATER 65535 OR part MATCHES "^0[0-9]")
        message(FATAL_ERROR "Build identity version components must be decimal integers from 0 to 65535.")
    endif()
endforeach()
if(NOT BUILD_ARCH MATCHES "^(x64|ARM64)$" OR NOT BUILD_CONFIG MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "Build identity requires x64/ARM64 and an ASCII configuration identifier.")
endif()
if(NOT DEFINED BUILD_FLAVOR)
    set(BUILD_FLAVOR voice-off)
endif()
if(NOT BUILD_FLAVOR MATCHES "^(voice-off|voice-portable|voice-avx2)$")
    message(FATAL_ERROR "Build identity requires a fixed supported voice flavor.")
endif()
if(NOT EXISTS "${SOURCE_DIR}/CMakeLists.txt" OR NOT IS_DIRECTORY "${SOURCE_DIR}/src")
    message(FATAL_ERROR "Build identity requires the Gamepad Desktop source directory.")
endif()

# Read local metadata at build time, not configure time. Never contact a remote.
find_program(GIT_EXECUTABLE NAMES git)
set(revision "unversioned")
if(GIT_EXECUTABLE)
    # An export inside another checkout must not inherit that checkout's HEAD.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
        ls-files --error-unmatch -- CMakeLists.txt
        RESULT_VARIABLE tracked_result OUTPUT_QUIET ERROR_QUIET TIMEOUT 5)
    if(tracked_result STREQUAL "0")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --verify HEAD
            RESULT_VARIABLE git_result OUTPUT_VARIABLE git_head
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 5)
        string(LENGTH "${git_head}" head_length)
        if(git_result STREQUAL "0" AND git_head MATCHES "^[0-9a-f]+$"
           AND (head_length EQUAL 40 OR head_length EQUAL 64))
            string(SUBSTRING "${git_head}" 0 12 short_head)
            set(revision "git.${short_head}")
        endif()
    endif()
endif()
if(revision STREQUAL "unversioned")
    message(WARNING "Gamepad Desktop build identity is unversioned: local Git/HEAD metadata for this source is unavailable.")
endif()

# Only app sources/resources and local CMake inputs; never build output or user settings.
file(GLOB_RECURSE inputs LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}"
    "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.h"
    "${SOURCE_DIR}/src/*.rc" "${SOURCE_DIR}/src/*.in"
    "${SOURCE_DIR}/third_party/PredictionVocabulary.inc"
    "${SOURCE_DIR}/cmake/*.cmake" "${SOURCE_DIR}/cmake/*.in"
    "${SOURCE_DIR}/cmake/*.ps1" "${SOURCE_DIR}/cmake/CMakeLists.txt")
list(APPEND inputs CMakeLists.txt)
list(SORT inputs)
set(source_hashes "")
foreach(input IN LISTS inputs)
    file(SHA256 "${SOURCE_DIR}/${input}" input_hash)
    string(APPEND source_hashes "${input}:${input_hash}\n")
endforeach()
string(SHA256 source_hash "${source_hashes}")
string(SUBSTRING "${source_hash}" 0 16 short_source_hash)
set(GAMEPAD_BUILD_LABEL "${APP_VERSION}+${revision}.src.${short_source_hash}.${BUILD_ARCH}.${BUILD_CONFIG}.${BUILD_FLAVOR}")
string(REPLACE "." "," GAMEPAD_BUILD_VERSION "${APP_VERSION}")

# configure_file copies only when content differs, preserving no-op build timestamps.
configure_file("${CMAKE_CURRENT_LIST_DIR}/BuildInfo.h.in" "${OUTPUT_FILE}" @ONLY)
