# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-FileCopyrightText: (C) 2012 - 2024 Symless Ltd
# SPDX-FileCopyrightText: (C) 2009 - 2012 Nick Bolton
# SPDX-License-Identifier: MIT

# The background service is available on Windows.
if(WIN32)
  set(target ${CMAKE_PROJECT_NAME}-daemon)

  # Generate rc file
  set(EXE_DESCRIPTION "${CMAKE_PROJECT_PROPER_NAME} background service")
  set(EXE_ICON "IDI_DESKFLOW ICON DISCARDABLE \"${CMAKE_SOURCE_DIR}/src/apps/res/deskflow.ico\"")
  configure_file(${CMAKE_SOURCE_DIR}/src/apps/res/windows.rc.in ${target}.rc)

  add_executable(
    ${target} WIN32
    ${target}.cpp
    DaemonApp.cpp
    DaemonApp.h
    ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc)

  target_link_libraries(
    ${target}
    arch
    base
    io
    mt
    net
    platform
    app
    common
    ${libs}
    Qt6::Core)

  install(
    TARGETS ${target}
    RUNTIME_DEPENDENCY_SET daemonDeps
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(RUNTIME_DEPENDENCY_SET daemonDeps
    ${WIN32_RUNTIME_DEPENDENCY_SET_ARGS}
    PRE_EXCLUDE_REGEXES ${WIN32_PRE_EXCLUDE_REGEXES}
    POST_EXCLUDE_REGEXES ${WIN32_POST_EXCLUDE_REGEXES}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()
