# The platform-independent core. No dependency on OBS, ReShade, ImGui or Win32, which is what
# makes the protocol, configuration, layout and notification lifecycle testable in CI on any host.
add_library(obsn_shared STATIC
  src/json.cpp
  src/model.cpp
  src/protocol.cpp
  src/framing.cpp
  src/config.cpp
  src/config_enums.cpp
  src/config_defaults.cpp
  src/layout.cpp
  src/notifications.cpp
  src/log.cpp
  src/overlay_client.cpp
  src/profile_store.cpp
  src/transport_win32.cpp
  src/transport_posix.cpp
)

target_include_directories(obsn_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(obsn_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(obsn_shared PUBLIC Threads::Threads)

if(WIN32)
  # advapi32 for the pipe security descriptor.
  target_link_libraries(obsn_shared PUBLIC advapi32)
  target_compile_definitions(obsn_shared PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# The commit the binary was built from, shown in the settings window. A screenshot of a bug
# report should say which build it is against; absent git is not an error, a source drop still
# builds and simply says "local".
find_package(Git QUIET)
set(OBSN_BUILD_ID "local")
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                  OUTPUT_VARIABLE OBSN_GIT_SHA
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET
                  RESULT_VARIABLE OBSN_GIT_RESULT)
  if(OBSN_GIT_RESULT EQUAL 0 AND OBSN_GIT_SHA)
    set(OBSN_BUILD_ID "${OBSN_GIT_SHA}")
  endif()
endif()
message(STATUS "OBS notification overlay build id: ${OBSN_BUILD_ID}")

target_compile_definitions(obsn_shared PUBLIC
  OBSN_VERSION="${PROJECT_VERSION}"
  OBSN_BUILD_ID="${OBSN_BUILD_ID}")
