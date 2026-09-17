if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
   OR NOT CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|x64)$")
    message(FATAL_ERROR "The isolated Whisper package requires an MSVC x64/ARM64 application.")
endif()
if(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded"
   OR NOT CMAKE_CONFIGURATION_TYPES STREQUAL "Release")
    message(FATAL_ERROR "The isolated Whisper package is Release /MT only. Configure with -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_CONFIGURATION_TYPES=Release.")
endif()
if(GAMEPAD_VOICE_AVX2)
    message(FATAL_ERROR "The isolated Whisper package is voice-portable; set GAMEPAD_VOICE_AVX2=OFF.")
endif()
if(NOT EXISTS "${GAMEPAD_WHISPER_ROOT}/manifest.cmake")
    message(FATAL_ERROR "Missing Whisper package manifest at ${GAMEPAD_WHISPER_ROOT}. Run cmake/BuildWhisperLibrary.ps1 first.")
endif()
include("${GAMEPAD_WHISPER_ROOT}/manifest.cmake")
if(NOT GAMEPAD_WHISPER_PACKAGE_VERSION STREQUAL "1"
   OR NOT GAMEPAD_WHISPER_ARCH STREQUAL CMAKE_GENERATOR_PLATFORM
   OR NOT GAMEPAD_WHISPER_COMMIT STREQUAL "a8d002cfd879315632a579e73f0148d06959de36"
   OR NOT GAMEPAD_WHISPER_ARCHIVE_SHA256 STREQUAL "4f02191b61135324a80137e459cd239e037ef3852bad4e6a2ea69490f3d89490"
   OR NOT GAMEPAD_WHISPER_LLVM STREQUAL "20.1.8"
   OR NOT GAMEPAD_WHISPER_CRT STREQUAL "MultiThreaded")
    message(FATAL_ERROR "Whisper package architecture/version/CRT mismatch. Rebuild with cmake/BuildWhisperLibrary.ps1 -Architecture ${CMAKE_GENERATOR_PLATFORM}.")
endif()
if(NOT GAMEPAD_WHISPER_MSVC_ABI MATCHES "^[0-9]+\\.[0-9]+$"
   OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS GAMEPAD_WHISPER_MSVC_ABI)
    message(FATAL_ERROR "Use an MSVC toolset at least as new as the Whisper package's Microsoft ABI ${GAMEPAD_WHISPER_MSVC_ABI}, or rebuild the dependency with the application's VS toolset.")
endif()
foreach(required lib/whisper.lib lib/ggml.lib lib/ggml-base.lib lib/ggml-cpu.lib
        include/whisper.h include/ggml.h include/ggml-cpu.h include/ggml-backend.h whisper.cpp-LICENSE.txt)
    if(NOT required IN_LIST GAMEPAD_WHISPER_FILES)
        message(FATAL_ERROR "Whisper package manifest is incomplete: ${required}")
    endif()
endforeach()
foreach(relative IN LISTS GAMEPAD_WHISPER_FILES)
    if(NOT EXISTS "${GAMEPAD_WHISPER_ROOT}/${relative}")
        message(FATAL_ERROR "Whisper package file missing: ${relative}")
    endif()
    file(SHA256 "${GAMEPAD_WHISPER_ROOT}/${relative}" actual_hash)
    if(NOT actual_hash STREQUAL "${GAMEPAD_WHISPER_SHA256_${relative}}")
        message(FATAL_ERROR "Whisper package integrity mismatch: ${relative}; rebuild the isolated library.")
    endif()
endforeach()
foreach(name ggml-base ggml-cpu ggml whisper)
    if(NOT EXISTS "${GAMEPAD_WHISPER_ROOT}/lib/${name}.lib")
        message(FATAL_ERROR "Incomplete Whisper package: lib/${name}.lib")
    endif()
    add_library(${name} STATIC IMPORTED)
    set_target_properties(${name} PROPERTIES
        IMPORTED_LOCATION "${GAMEPAD_WHISPER_ROOT}/lib/${name}.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${GAMEPAD_WHISPER_ROOT}/include")
endforeach()
set_target_properties(ggml-cpu PROPERTIES INTERFACE_LINK_LIBRARIES ggml-base)
set_target_properties(ggml PROPERTIES INTERFACE_LINK_LIBRARIES "ggml-cpu;ggml-base")
set_target_properties(whisper PROPERTIES INTERFACE_LINK_LIBRARIES ggml)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/voice-licenses")
configure_file("${GAMEPAD_WHISPER_ROOT}/whisper.cpp-LICENSE.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/voice-licenses/whisper.cpp-LICENSE.txt" COPYONLY)
message(STATUS "Local voice: isolated clang-cl ${GAMEPAD_WHISPER_LLVM} ${GAMEPAD_WHISPER_ARCH} Release /MT static libraries; application compiler remains MSVC")
