/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "mt/Thread.h"
#include "platform/MSWindowsHandle.h"
#include "platform/MSWindowsProcess.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

typedef VOID(WINAPI *SendSas)(BOOL asUser);

class FileLogOutputter;

/**
 * @brief Monitors and controls a core process in the requesting Windows session.
 */
class MSWindowsWatchdog
{
  enum class ProcessState
  {
    Idle,
    StartScheduled,
    StartPending,
    StopPending,
    Running
  };

public:
  explicit MSWindowsWatchdog(bool foreground, FileLogOutputter &fileLogOutputter);
  ~MSWindowsWatchdog() = default;

  /**
   * @brief Start threads for main loop and and output loop.
   */
  void startAsync();

  /**
   * @brief Set the command and client token, or stop the current child with an empty command.
   */
  void setProcessConfig(
      std::wstring command, std::shared_ptr<MSWindowsHandle> token = {}, DWORD sessionId = 0,
      std::function<void(bool)> onStarted = {}
  );

  /**
   * @brief Stop the main loop and output loop threads.
   */
  void stop();

  /**
   * @return True if the process is running.
   */
  bool isProcessRunning();

private:
  /**
   * @brief Monitor the process state and start/stop the process as necessary.
   */
  void mainLoop(const void *);

  /**
   * @brief Monitor the process standard out/error and write to the file log outputter.
   */
  void outputLoop(const void *);

  void startProcess();

  /**
   * @brief Controls whether the process should restart immediately or delay start.
   */
  ProcessState handleStartError(const std::string_view &message = "");

  /**
   * @brief Init the output read pipe for standard out/error.
   */
  void initOutputReadPipe();

  /**
   * @brief Init the SendSAS function, used for Ctrl+Alt+Del emulation.
   */
  void initSasFunc();

  /**
   * @brief Allows the SendSAS function to be called from other processes.
   *
   * SendSAS sends a SAS (Secure Attention Sequence) for Ctrl+Alt+Del emulation.
   */
  void sasLoop(const void *);

  /**
   * @brief Convert the process state enum to a string (useful for logging).
   */
  static std::string processStateToString(ProcessState state);

private:
  std::atomic_bool m_running = true;
  std::unique_ptr<Thread> m_mainThread;
  std::unique_ptr<Thread> m_outputThread;
  std::unique_ptr<Thread> m_sasThread;
  HANDLE m_outputWritePipe = nullptr;
  HANDLE m_outputReadPipe = nullptr;
  std::shared_ptr<MSWindowsHandle> m_userToken;
  DWORD m_sessionId = 0;
  std::function<void(bool)> m_onStarted;
  int m_startFailures = 0;
  FileLogOutputter &m_fileLogOutputter;
  bool m_foreground = false;
  std::wstring m_activeDesktop = {};
  std::unique_ptr<deskflow::platform::MSWindowsProcess> m_process;
  std::optional<double> m_nextStartTime = std::nullopt;
  ProcessState m_processState = ProcessState::Idle;
  std::wstring m_command = {};
  SendSas m_sendSasFunc = nullptr;
  std::mutex m_processStateMutex;
};
