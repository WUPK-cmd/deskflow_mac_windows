# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

# Explicit file logging also works when a Windows GUI test has no stdout.
file(REMOVE "${TEST_OUTPUT}")
execute_process(
  COMMAND "${TEST_EXECUTABLE}" -o "${TEST_OUTPUT},txt"
  RESULT_VARIABLE test_result
)

if(EXISTS "${TEST_OUTPUT}")
  file(READ "${TEST_OUTPUT}" test_output)
  message("${test_output}")
endif()

if(NOT "${test_result}" STREQUAL "0")
  message(FATAL_ERROR "${TEST_EXECUTABLE} exited with code ${test_result}")
endif()
