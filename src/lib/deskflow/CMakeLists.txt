# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-FileCopyrightText: (C) 2024 - 2026 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-FileCopyrightText: (C) 2012 - 2025 Symless Ltd
# SPDX-FileCopyrightText: (C) 2009 - 2012 Nick Bolton
# SPDX-License-Identifier: MIT

set(lib_name app)

# arch
if(WIN32)
  set(PLATFORM_CODE
    win32/AppUtilWindows.cpp
    win32/AppUtilWindows.h
    ipc/DaemonIpcServer.cpp
    ipc/DaemonIpcServer.h
  )
elseif(UNIX)
  set(PLATFORM_CODE
    unix/AppUtilUnix.cpp
    unix/AppUtilUnix.h
    unix/DeskflowXkbKeyboard.cpp
    unix/DeskflowXkbKeyboard.h
    unix/ISO639Table.h
    unix/XkbLayoutsParser.cpp
    unix/XkbLayoutsParser.h
  )
endif()

add_library(${lib_name} STATIC ${PLATFORM_CODE}
  App.cpp
  App.h
  AppUtil.cpp
  AppUtil.h
  Chunk.cpp
  Chunk.h
  ClientApp.cpp
  ClientApp.h
  ClipboardTypes.h
  Clipboard.cpp
  Clipboard.h
  ClipboardChunk.cpp
  ClipboardChunk.h
  Computer.cpp
  Computer.h
  DeskflowException.cpp
  DeskflowException.h
  DisplayException.cpp
  DisplayException.h
  DisplayInvalidException.h
  IApp.h
  IClient.h
  IClipboard.cpp
  IClipboard.h
  IComputer.h
  IKeyState.cpp
  IKeyState.h
  IPlatformComputer.h
  IPrimaryComputer.cpp
  IPrimaryComputer.h
  IScreenSaver.h
  ISecondaryComputer.h
  KeyTypes.cpp
  KeyTypes.h
  KeyMap.cpp
  KeyMap.h
  KeyState.cpp
  KeyState.h
  MouseTypes.h
  Microphone.cpp
  Microphone.h
  OptionTypes.h
  PacketStreamFilter.cpp
  PacketStreamFilter.h
  PlatformComputer.cpp
  PlatformComputer.h
  ProtocolTypes.cpp
  ProtocolTypes.h
  ProtocolUtil.cpp
  ProtocolUtil.h
  ServerApp.cpp
  ServerApp.h
  StreamChunker.cpp
  StreamChunker.h
  KeyboardLayoutManager.cpp
  KeyboardLayoutManager.h
  ipc/IpcServer.cpp
  ipc/IpcServer.h
  ipc/CoreIpc.cpp
  ipc/CoreIpc.h
  ipc/CoreIpcServer.cpp
  ipc/CoreIpcServer.h
)

target_link_libraries(${lib_name}
  PUBLIC
    common
    Qt6::Network
  PRIVATE
    platform
)

if(UNIX AND NOT APPLE)
  target_link_libraries(${lib_name} PRIVATE server)
endif()

if(BUILD_MICROPHONE_FOLLOW)
  target_compile_definitions(${lib_name} PRIVATE DESKFLOW_MICROPHONE)
  target_link_libraries(${lib_name} PRIVATE Qt6::Multimedia)
  if(WIN32)
    target_link_libraries(${lib_name} PRIVATE ole32)
  endif()
endif()
