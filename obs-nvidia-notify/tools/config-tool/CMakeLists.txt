# A small command-line front end to the same loader the add-on uses.
#
# It exists so the example profiles are validated in CI by the real implementation rather than a
# second one written in another language, and so a user can check a hand-edited profile without
# launching a game.
add_executable(obsn_config src/main.cpp)
target_link_libraries(obsn_config PRIVATE obsn_shared)
set_target_properties(obsn_config PROPERTIES OUTPUT_NAME "obsn-config")
