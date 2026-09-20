add_executable(obsn_tests
  main.cpp
  test_json.cpp
  test_protocol.cpp
  test_framing.cpp
  test_config.cpp
  test_layout.cpp
  test_motion.cpp
  test_notifications.cpp
  test_model.cpp
  test_triple_buffer.cpp
  test_profile_store.cpp
  test_ipc_integration.cpp
)

target_link_libraries(obsn_tests PRIVATE obsn_shared)
target_include_directories(obsn_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# One ctest entry per suite, so a failure names the area rather than just "tests failed".
foreach(suite json protocol framing config layout motion notifications model triple_buffer profiles ipc)
  add_test(NAME ${suite} COMMAND obsn_tests ${suite})
endforeach()
