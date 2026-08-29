cmake_minimum_required(VERSION 3.22)

set(TARGET UNBSEMod)
project(${TARGET} LANGUAGES CXX)

get_filename_component(UNBSE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(${TARGET} SHARED
    src/AddonRegistry.cpp
    src/CoreLuaBinding.cpp
    src/core_dllmain.cpp
    src/ScriptServiceRegistry.cpp
    src/UNBSECoreMod.cpp
)

target_include_directories(${TARGET} PRIVATE
    include
    "${UNBSE_SOURCE_ROOT}/include"
)
target_compile_features(${TARGET} PRIVATE cxx_std_23)
target_compile_definitions(${TARGET} PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(${TARGET} PUBLIC UE4SS)
if(MSVC)
    # Keep Shipping packages reproducible across clean build directories.
    # /Brepro makes compiler and linker identities content-derived.
    target_compile_options(${TARGET} PRIVATE "/Brepro")
    target_link_options(${TARGET} PRIVATE "/Brepro")

    # UE4SS applies common link options through its interface. Keep debug
    # linking for development targets, but suppress it for the packaged UNBSE
    # DLL because its PDB is not shipped and its debug directory must not carry
    # build-root-specific CodeView provenance.
    function(unbse_finalize_reproducible_link)
        foreach(UNBSE_PROPERTY_TARGET_PROPERTY IN ITEMS
                "UE4SS|INTERFACE_LINK_OPTIONS"
                "UNBSEMod|LINK_OPTIONS")
            string(REPLACE "|" ";" UNBSE_PROPERTY_TARGET_PROPERTY_PARTS
                "${UNBSE_PROPERTY_TARGET_PROPERTY}")
            list(GET UNBSE_PROPERTY_TARGET_PROPERTY_PARTS 0 UNBSE_PROPERTY_TARGET)
            list(GET UNBSE_PROPERTY_TARGET_PROPERTY_PARTS 1 UNBSE_PROPERTY)
            get_target_property(UNBSE_OPTIONS ${UNBSE_PROPERTY_TARGET} ${UNBSE_PROPERTY})
            if(UNBSE_OPTIONS)
                string(REPLACE
                    "$<$<STREQUAL:$<CONFIG>,Game__Shipping__Win64>:/DEBUG:FULL;"
                    "$<$<STREQUAL:$<CONFIG>,Game__Shipping__Win64>:"
                    UNBSE_OPTIONS "${UNBSE_OPTIONS}")
                set_property(TARGET ${UNBSE_PROPERTY_TARGET} PROPERTY ${UNBSE_PROPERTY}
                    "${UNBSE_OPTIONS}")
            endif()
        endforeach()
        target_link_options(UNBSEMod PRIVATE
            "$<$<STREQUAL:$<CONFIG>,Game__Shipping__Win64>:/DEBUG:NONE>")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
        CALL unbse_finalize_reproducible_link)
endif()

set_target_properties(${TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "main"
    FOLDER "mods/UNBSE"
)
