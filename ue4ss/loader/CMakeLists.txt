cmake_minimum_required(VERSION 3.22)

project(UNBSELoader LANGUAGES CXX)

add_executable(UNBSELoader src/main.cpp)
target_compile_features(UNBSELoader PRIVATE cxx_std_20)
target_compile_definitions(UNBSELoader PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(UNBSELoader PRIVATE advapi32 bcrypt psapi)

if (MSVC)
    target_compile_options(UNBSELoader PRIVATE /Brepro /EHsc /permissive- /W4 /WX)
    target_link_options(UNBSELoader PRIVATE /Brepro /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
endif()
