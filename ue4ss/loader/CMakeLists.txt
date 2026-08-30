cmake_minimum_required(VERSION 3.22)

project(UNBSELoader LANGUAGES CXX)

set(UNBSE_EXPECTED_UE4SS_SHA256 ""
    CACHE STRING "SHA-256 of the packaged patched UE4SS host")
set(UNBSE_EXPECTED_UE4SS_BYTES "0"
    CACHE STRING "Byte size of the packaged patched UE4SS host")
string(LENGTH "${UNBSE_EXPECTED_UE4SS_SHA256}" UNBSE_EXPECTED_UE4SS_SHA256_LENGTH)
if(NOT UNBSE_EXPECTED_UE4SS_SHA256 MATCHES "^[0-9A-Fa-f]+$" OR
   NOT UNBSE_EXPECTED_UE4SS_SHA256_LENGTH EQUAL 64 OR
   NOT UNBSE_EXPECTED_UE4SS_BYTES MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "Pass the built patched UE4SS hash and byte size to configure the launcher")
endif()

set(UNBSE_OBSE64_INTEROP_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../mod/OBSE64Interop")

add_executable(UNBSELoader
    src/main.cpp
    "${UNBSE_OBSE64_INTEROP_ROOT}/src/OBSE64PluginScanner.cpp"
)
target_include_directories(UNBSELoader PRIVATE
    "${UNBSE_OBSE64_INTEROP_ROOT}/include"
)
target_compile_features(UNBSELoader PRIVATE cxx_std_20)
target_compile_definitions(UNBSELoader PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNBSE_EXPECTED_UE4SS_SHA256="${UNBSE_EXPECTED_UE4SS_SHA256}"
    UNBSE_EXPECTED_UE4SS_BYTES=${UNBSE_EXPECTED_UE4SS_BYTES}
)
target_link_libraries(UNBSELoader PRIVATE advapi32 bcrypt psapi user32)

if (MSVC)
    target_compile_options(UNBSELoader PRIVATE /Brepro /EHsc /permissive- /W4 /WX)
    target_link_options(UNBSELoader PRIVATE /Brepro /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
endif()
