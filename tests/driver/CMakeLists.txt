add_test(
  NAME python-driver-tests
  COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/driver_tests.py
          $<TARGET_FILE:rtl-wiring-inspector>
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(
  NAME python-loader-tests
  COMMAND ${Python_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/test_loader_validation.py
          $<TARGET_FILE:rtl-wiring-inspector>
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

set_tests_properties(
  python-driver-tests python-loader-tests
  PROPERTIES ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/tests/utilities")
