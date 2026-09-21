cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "Build option tests require SOURCE_DIR.")
endif()
foreach(suffix VOICE_WHISPER VOICE_AVX2 WHISPER_ROOT CONTROLLER_DATABASE_ROOT LLVM_ROOT WHISPER_ARCH WHISPER_ARCHIVE)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DGAMEPAD_${suffix}=test"
        -P "${SOURCE_DIR}/cmake/BuildOptions.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result STREQUAL "0" OR NOT error MATCHES "OVERWORLD_${suffix}")
        message(FATAL_ERROR "Legacy ${suffix} must fail explicitly with its new spelling, not silently lose requested behavior.")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DOVERWORLD_${suffix}=test"
        -P "${SOURCE_DIR}/cmake/BuildOptions.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Overworld option rejected: ${output}${error}")
    endif()
endforeach()
message(STATUS "Overworld options accepted; legacy build/cache spellings rejected explicitly.")
