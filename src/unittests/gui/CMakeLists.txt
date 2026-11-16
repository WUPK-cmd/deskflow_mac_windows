# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

create_test(
  NAME MainWindowPresentationTests
  DEPENDS gui
  SOURCE MainWindowPresentationTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_target_properties(MainWindowPresentationTests PROPERTIES
  AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/src/lib/gui"
)

create_test(
  NAME DesktopWorkflowTests
  DEPENDS gui
  SOURCE DesktopWorkflowTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME MicrophoneSettingsTests
  DEPENDS gui
  SOURCE MicrophoneSettingsTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME VersionCheckerTests
  DEPENDS gui
  SOURCE VersionCheckerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
