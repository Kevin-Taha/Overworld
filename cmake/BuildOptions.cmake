include_guard(GLOBAL)

# An old voice-ON cache must not silently become a voice-OFF Overworld build.
foreach(suffix VOICE_WHISPER VOICE_AVX2 WHISPER_ROOT CONTROLLER_DATABASE_ROOT LLVM_ROOT WHISPER_ARCH WHISPER_ARCHIVE)
    if(DEFINED GAMEPAD_${suffix})
        message(FATAL_ERROR "Legacy build option GAMEPAD_${suffix} is not supported. Use OVERWORLD_${suffix} in a fresh build directory.")
    endif()
endforeach()
