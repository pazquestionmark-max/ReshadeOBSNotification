# The ReShade add-on.
#
# ReShade resolves the Dear ImGui function table by exact IMGUI_VERSION_NUM, so the ReShade SDK
# and ImGui versions must be a matched pair -- scripts/fetch-deps pins them together and the
# checks below fail the build loudly rather than producing an add-on that loads and then
# silently draws nothing.

set(OBSN_RESHADE_DIR "${CMAKE_SOURCE_DIR}/third_party/reshade"
    CACHE PATH "Path to a checkout of crosire/reshade (headers only are needed)")
set(OBSN_IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui"
    CACHE PATH "Path to a checkout of ocornut/imgui at the version ReShade expects")

if(NOT EXISTS "${OBSN_RESHADE_DIR}/include/reshade.hpp")
  message(FATAL_ERROR
    "ReShade SDK headers not found under '${OBSN_RESHADE_DIR}'.\n"
    "Run scripts/fetch-deps.ps1 (or fetch-deps.sh), or pass -DOBSN_RESHADE_DIR=<path>.")
endif()
if(NOT EXISTS "${OBSN_IMGUI_DIR}/imgui.h")
  message(FATAL_ERROR
    "Dear ImGui headers not found under '${OBSN_IMGUI_DIR}'.\n"
    "Run scripts/fetch-deps.ps1 (or fetch-deps.sh), or pass -DOBSN_IMGUI_DIR=<path>.")
endif()

# Verify the pairing at configure time.
file(STRINGS "${OBSN_IMGUI_DIR}/imgui.h" OBSN_IMGUI_VERSION_LINE
     REGEX "^#define IMGUI_VERSION_NUM" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+" OBSN_IMGUI_VERSION "${OBSN_IMGUI_VERSION_LINE}")
file(STRINGS "${OBSN_RESHADE_DIR}/include/reshade_overlay.hpp" OBSN_RESHADE_IMGUI_LINE
     REGEX "IMGUI_VERSION_NUM !=" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+" OBSN_RESHADE_IMGUI_VERSION "${OBSN_RESHADE_IMGUI_LINE}")

if(NOT OBSN_IMGUI_VERSION STREQUAL OBSN_RESHADE_IMGUI_VERSION)
  message(FATAL_ERROR
    "Dear ImGui version mismatch: the ReShade SDK expects IMGUI_VERSION_NUM "
    "${OBSN_RESHADE_IMGUI_VERSION} but '${OBSN_IMGUI_DIR}' is ${OBSN_IMGUI_VERSION}.\n"
    "ReShade hands add-ons a function table for one exact ImGui ABI. Building against a "
    "different one produces an add-on that fails to load. Re-run scripts/fetch-deps to get the "
    "pinned pair, or pass matching --reshade-tag and --imgui-tag.")
endif()
# ReShade's imgui_function_table declares DockSpace / ImGuiDockNodeFlags / ImGuiWindowClass,
# which exist only on Dear ImGui's docking branch. Against master the add-on does not compile,
# so check for it here where the message can say why.
file(STRINGS "${OBSN_IMGUI_DIR}/imgui.h" OBSN_IMGUI_DOCKING
     REGEX "IMGUI_HAS_DOCK" LIMIT_COUNT 1)
if(NOT OBSN_IMGUI_DOCKING)
  message(FATAL_ERROR
    "'${OBSN_IMGUI_DIR}' is not Dear ImGui's docking branch.\n"
    "ReShade's add-on ImGui function table declares DockSpace, ImGuiDockNodeFlags and "
    "ImGuiWindowClass, which exist only on the docking branch. Re-run scripts/fetch-deps, or "
    "pass --imgui-tag v<version>-docking.")
endif()
message(STATUS "ReShade add-on: Dear ImGui ${OBSN_IMGUI_VERSION} (docking) matches the ReShade SDK")

add_library(obsn_addon SHARED
  src/addon.cpp
  src/font_engine.cpp
  src/renderer.cpp
  src/settings_ui.cpp
  src/icons.cpp
)

target_include_directories(obsn_addon
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${OBSN_RESHADE_DIR}/include
  PRIVATE ${OBSN_IMGUI_DIR}
)

# No ImGui .cpp is compiled or linked: ReShade's reshade_overlay.hpp supplies inline definitions
# for the whole ImGui:: namespace *and* for ImDrawList::/ImFont:: members, all routed through the
# function table ReShade hands the add-on. imgui.h is included for the type definitions and the
# declarations those inline definitions attach to -- which is why every source that calls ImGui
# must include reshade.hpp as well, or it links against nothing.
target_compile_definitions(obsn_addon PRIVATE
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  IMGUI_DISABLE_DEMO_WINDOWS
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_link_libraries(obsn_addon PRIVATE obsn_shared)

# ReShade loads add-ons by extension: .addon64 for 64-bit, .addon for 32-bit.
set_target_properties(obsn_addon PROPERTIES
  OUTPUT_NAME "OBSNotifications"
  PREFIX ""
  SUFFIX ".addon64"
)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_target_properties(obsn_addon PROPERTIES SUFFIX ".addon")
endif()

install(TARGETS obsn_addon RUNTIME DESTINATION addon LIBRARY DESTINATION addon)
