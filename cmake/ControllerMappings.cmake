# Generic SDL-format mapping data only; no SDL UI/runtime or device-specific app code.
include("${CMAKE_CURRENT_LIST_DIR}/BuildOptions.cmake")
set(OVERWORLD_CONTROLLER_DATABASE_ROOT "" CACHE PATH "Offline directory containing pinned gamecontrollerdb.txt and LICENSE")
set(controller_database_revision 5a12daa568d19344f9b6e9286ef5929833b25c7c)
if(OVERWORLD_CONTROLLER_DATABASE_ROOT)
    set(controller_database_root "${OVERWORLD_CONTROLLER_DATABASE_ROOT}")
else()
    set(controller_database_root "${CMAKE_BINARY_DIR}/controller-database")
endif()
foreach(entry
    "gamecontrollerdb.txt|07ec5b753e685c4829987919b27905cc3c45c8e18c11b3046e275fcf38b0f3cb"
    "LICENSE|97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127")
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 name)
    list(GET pair 1 hash)
    set(path "${controller_database_root}/${name}")
    if(NOT EXISTS "${path}" AND NOT OVERWORLD_CONTROLLER_DATABASE_ROOT)
        file(MAKE_DIRECTORY "${controller_database_root}")
        file(DOWNLOAD "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/${controller_database_revision}/${name}"
            "${path}" EXPECTED_HASH "SHA256=${hash}" TLS_VERIFY ON TIMEOUT 60)
    endif()
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing controller mapping input: ${name}")
    endif()
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL hash)
        message(FATAL_ERROR "Controller mapping input hash mismatch: ${name}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/controller-licenses")
configure_file("${controller_database_root}/LICENSE"
    "${CMAKE_BINARY_DIR}/controller-licenses/SDL_GameControllerDB-LICENSE.txt" COPYONLY)
function(overworld_stage_controller_data target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/controllers"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${controller_database_root}/gamecontrollerdb.txt"
            "$<TARGET_FILE_DIR:${target}>/controllers/gamecontrollerdb.txt"
        VERBATIM)
endfunction()
