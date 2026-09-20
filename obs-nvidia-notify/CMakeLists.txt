cmake_minimum_required(VERSION 3.20)
project(obs-nvidia-notify VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OBSN_BUILD_TESTS "Build the unit and integration test suite" ON)
option(OBSN_BUILD_ADDON "Build the ReShade add-on (Windows only)" OFF)
option(OBSN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CRT for every target, set once here rather than per target. The add-on needs it -- a
# DLL that depends on a redistributable the user has not installed simply fails to load, with no
# diagnostic they can act on -- and a static library built against the dynamic CRT cannot be
# linked into it (LNK2038). Setting it in one place is what stops those drifting.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  if(OBSN_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(OBSN_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

add_subdirectory(shared)
add_subdirectory(tools/config-tool)

if(OBSN_BUILD_ADDON)
  add_subdirectory(reshade-integration)
endif()

if(OBSN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
