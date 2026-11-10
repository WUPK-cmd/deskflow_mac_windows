/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsWatchdog.h"

#include "arch/Arch.h"
#include "arch/win32/XArchWindows.h"
#include "base/Log.h"
#include "base/LogOutputters.h"
#include "base/TMethodJob.h"
#include "common/Constants.h"
#include "common/LogLevel.h"
#include "deskflow/App.h"
#include "mt/Thread.h"
#include "platform/MSWindowsHandle.h"

#include <Shellapi.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shlobj.h>
#include <tchar.h>

#include <QStringDecoder>
#include <utility>

//
// MSWindowsWatchdog
//

MSWindowsWatchdog::MSWindowsWatchdog(bool foreground, FileLogOutputter &fileLogOutputter)
    : m_fileLogOutputter(fileLogOutputter),
      m_foreground(foreground)
{
  initSasFunc();
  initOutputReadPipe();
}

void MSWindowsWatchdog::startAsync()
{
  m_mainThread = std::make_unique<Thread>(new TMethodJob(this, &MSWindowsWatchdog::mainLoop, nullptr));
  m_outputThread = std::make_unique<Thread>(new TMethodJob(this, &MSWindowsWatchdog::outputLoop, nullptr));
  m_sasThread = std::make_unique<Thread>(new TMethodJob(this, &MSWindowsWatchdog::sasLoop, nullptr));
}

void MSWindowsWatchdog::stop()
{
  const auto kThreadWaitSeconds = 5;

  m_running = false;

  m_mainThread->wait();

  if (!m_outputThread->wait(kThreadWaitSeconds)) {
    LOG_WARN("could not stop output thread");
  }

  if (!m_sasThread->wait(kThreadWaitSeconds)) {
    LOG_WARN("could not stop sas thread");
  }
}

void MSWindowsWatchdog::mainLoop(const void *)
{
  using enum ProcessState;

  LOG_DEBUG("starting watchdog main loop");
  while (m_running) {
    LOG_VERBOSE("locking process state mutex in watchdog main loop");
    std::unique_lock lock(m_processStateMutex);

    if (!m_command.empty() && !m_foreground && m_sessionId != WTSGetActiveConsoleSessionId()) {
      LOG_DEBUG("session changed, stopping its core process");
      m_command.clear();
      m_processState = StopPending;
    }

    switch (m_processState) {
    case Idle:
      LOG_VERBOSE("watchdog process state idle");
      break;

    case StartScheduled: {
      LOG_VERBOSE("watchdog process start scheduled");
      if (m_nextStartTime.has_value() && m_nextStartTime.value() <= Arch::time()) {
        LOG_DEBUG("start time reached, queueing process start");
        m_processState = StartPending;
      }
    } break;

    case StartPending: {
      LOG_DEBUG("watchdog starting new process");
      try {
        startProcess();
        m_startFailures = 0;
        m_processState = Running;
        if (auto callback = std::exchange(m_onStarted, {}))
          callback(true);
      } catch (std::exception &e) { // NOSONAR - Catching all exceptions
        m_processState = handleStartError(e.what());
      } catch (...) { // NOSONAR - Catching remaining exceptions
        m_processState = handleStartError();
      }
    } break;

    case Running: {
      LOG_VERBOSE("watchdog process in running state");
      if (!isProcessRunning()) {
        LOG_WARN("detected application not running, pid=%d", m_process->info().dwProcessId);
        m_processState = StartPending;
      }
    } break;

    case StopPending: {
      LOG_DEBUG("watchdog stopping current process");
      if (m_process != nullptr) {
        m_process->shutdown();
        m_process.reset();
      } else {
        LOG_WARN("no process to stop");
      }
      m_userToken.reset();
      if (auto callback = std::exchange(m_onStarted, {}))
        callback(false);
      m_processState = Idle;
    } break;
    }

    LOG_VERBOSE("unlocking process state mutex in watchdog main loop");
    lock.unlock();

    // Sleep for only 100ms rather than 1 second so that the service can shut down faster.
    LOG_VERBOSE("watchdog main loop sleeping");
    Arch::sleep(0.1);
  }

  LOG_DEBUG("watchdog main loop finished");

  if (m_process != nullptr) {
    LOG_DEBUG("terminating running process on exit");
    m_process->shutdown();
    m_process.reset();
  }
}

bool MSWindowsWatchdog::isProcessRunning()
{
  if (m_process == nullptr) {
    return false;
  }

  DWORD exitCode = 0;
  return GetExitCodeProcess(m_process->info().hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

void MSWindowsWatchdog::startProcess()
{
  if (m_command.empty()) {
    throw std::runtime_error("cannot start process, command is empty");
  }

  if (m_process != nullptr) {
    LOG_DEBUG("closing existing process to make way for new one");
    m_process->shutdown();
    m_process.reset();
  }

  m_process = std::make_unique<deskflow::platform::MSWindowsProcess>(m_command, m_outputWritePipe, m_outputWritePipe);

  LOG_INFO("running command: %s", qUtf8Printable(QString::fromStdWString(m_command)));

  BOOL createRet;
  if (m_foreground) {
    LOG_DEBUG("starting command in foreground");
    createRet = m_process->startInForeground();
  } else {
    LOG_DEBUG("starting new process in user session");

    if (!m_userToken || !m_userToken->get())
      throw std::runtime_error("no authenticated user token");

    createRet = m_process->startAsUser(m_userToken->get(), nullptr);
  }

  if (!createRet)
    throw std::runtime_error(windowsErrorToString(GetLastError()));

  // Give the child time to report an immediate startup failure.
  Arch::sleep(1);
  if (!isProcessRunning()) {
    m_process.reset();
    throw std::runtime_error("process immediately stopped");
  }

  LOG_DEBUG("started core process from watchdog");
}

void MSWindowsWatchdog::setProcessConfig(
    std::wstring command, std::shared_ptr<MSWindowsHandle> token, DWORD sessionId, std::function<void(bool)> onStarted
)
{
  LOG_VERBOSE("locking process state mutex for watchdog config change");
  std::scoped_lock lock{m_processStateMutex};

  LOG_DEBUG("setting watchdog process config");
  if (auto callback = std::exchange(m_onStarted, {}))
    callback(false);
  m_command = std::move(command);
  m_userToken = std::move(token);
  m_sessionId = sessionId;
  m_onStarted = std::move(onStarted);

  if (m_command.empty()) {
    LOG_DEBUG("command cleared, queueing process stop");
    m_processState = ProcessState::StopPending;
  } else {
    LOG_DEBUG("command changed, queueing process start");
    m_processState = ProcessState::StartPending;
    m_nextStartTime.reset();
  }
}

void MSWindowsWatchdog::outputLoop(const void *)
{
  static constexpr DWORD kBufSize = 4096;

  BYTE raw[kBufSize];
  DWORD bytesRead = 0;

  // Warning: Manual decoding while we still use Win32 APIs for process launching and output reading.
  // Using a byte decoder should help to prevent mojibake when we get a partial UTF-8 sequence in a read chunk.
  // In future when we move to Qt process APIs, this can all be simplified.
  QStringDecoder decoder(QStringDecoder::Utf8);

  while (m_running) {
    const BOOL ok = ::ReadFile(m_outputReadPipe, raw, kBufSize, &bytesRead, nullptr);

    if (!ok || bytesRead == 0) {
      const DWORD err = ::GetLastError();
      if (err != NO_ERROR && err != ERROR_NO_DATA) {
        LOG_WARN("could not read from output pipe, error: %s", windowsErrorToString(err).c_str());
      }

      // Retry immediately when nothing to read or we get a transient error like ERROR_NO_DATA (pipe is non-blocking).
      Arch::sleep(0.1);
      continue;
    }

    const QString decoded =
        decoder.decode(QByteArray::fromRawData(reinterpret_cast<const char *>(raw), int(bytesRead)));

    // The file log outputter adds its own newlines, so trim the decoded string to avoid double newlines.
    const auto trimmed = decoded.trimmed();
    m_fileLogOutputter.write(LogLevel::Level::Print, trimmed);

    if (m_foreground) {
      // Doesn't add it's own newlines, so use the original ones from the process output.
      ::OutputDebugString(decoded.toStdWString().c_str());
    }
  }
}

MSWindowsWatchdog::ProcessState MSWindowsWatchdog::handleStartError(const std::string_view &message)
{
  const auto kStartDelaySeconds = 1;

  m_startFailures++;

  if (!message.empty()) {
    LOG_CRIT("daemon failed to start process, error: %s", message.data());
  } else {
    LOG_CRIT("daemon failed to start process, unknown error");
  }

  if (auto callback = std::exchange(m_onStarted, {})) {
    callback(false);
    m_command.clear();
    return ProcessState::StopPending;
  }

  // When there has been more than one consecutive failure, slow down the retry rate.
  if (m_startFailures > 1) {
    m_nextStartTime = Arch::time() + kStartDelaySeconds;
    LOG_WARN("start failed %d times, delaying start", m_startFailures);
    LOG_DEBUG("start delay, seconds=%d, time=%f", kStartDelaySeconds, m_nextStartTime.value());
    return ProcessState::StartScheduled;
  } else {
    LOG_INFO("retrying process start immediately");
    m_nextStartTime.reset();
    return ProcessState::StartPending;
  }
}

std::string MSWindowsWatchdog::processStateToString(MSWindowsWatchdog::ProcessState state)
{
  switch (state) {
    using enum MSWindowsWatchdog::ProcessState;

  case Idle:
    return "Idle";
  case StartScheduled:
    return "StartScheduled";
  case StartPending:
    return "StartPending";
  case StopPending:
    return "StopPending";
  case Running:
    return "Running";
  }

  return "Unknown";
}

void MSWindowsWatchdog::initOutputReadPipe()
{
  SECURITY_ATTRIBUTES saAttr;
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = nullptr;

  if (!CreatePipe(&m_outputReadPipe, &m_outputWritePipe, &saAttr, 0)) {
    LOG_ERR("could not create output pipe");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  // Set the pipe to non-blocking mode, which allows us to stop the output reader thread immediately
  // in order to speed up the shutdown process when the Windows service needs to stop.
  if (DWORD mode = PIPE_NOWAIT; !SetNamedPipeHandleState(m_outputReadPipe, &mode, nullptr, nullptr)) {
    LOG_ERR("could not set pipe to non-blocking mode");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }
}

void MSWindowsWatchdog::initSasFunc()
{
  // the SendSAS function is used to send a sas (secure attention sequence) to the
  // winlogon process. this is used to switch to the login screen.
  HINSTANCE sasLib = LoadLibrary(L"sas.dll");
  if (!sasLib) {
    LOG_ERR("could not load sas.dll");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  LOG_DEBUG("loaded sas.dll, used to simulate ctrl-alt-del");
  m_sendSasFunc = (SendSas)GetProcAddress(sasLib, "SendSAS");
  if (!m_sendSasFunc) {
    LOG_ERR("could not find SendSAS function in sas.dll");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  LOG_DEBUG("found SendSAS function in sas.dll");
}

void MSWindowsWatchdog::sasLoop(const void *) // NOSONAR - Thread entry point signature
{
  LOG_VERBOSE("watchdog creating sas event");

  if (m_sendSasFunc == nullptr) {
    throw std::runtime_error("SendSAS function not initialized");
  }

  while (m_running) {
    bool coreRunning;
    {
      std::scoped_lock lock{m_processStateMutex};
      coreRunning = m_processState == ProcessState::Running;
    }
    if (!coreRunning) {
      LOG_VERBOSE("watchdog not running, skipping SendSAS");
      Arch::sleep(1);
      continue;
    }

    // Create a an event so that other processes can tell the daemon to call the `SendSAS` function.
    MSWindowsHandle sendSasEvent(CreateEvent(nullptr, FALSE, FALSE, LPCWSTR(kSendSasEventName)));
    if (sendSasEvent.get() == nullptr) {
      LOG_ERR("could not create SAS event, error: %s", windowsErrorToString(GetLastError()).c_str());
      Arch::sleep(1);
      continue;
    }

    // Wait for the Core client to tell the daemon to call the `SendSAS` function.
    if (WaitForSingleObject(sendSasEvent.get(), 1000) == WAIT_OBJECT_0) {

      // The SoftwareSASGeneration registry key must be set for this to work:
      //   Set-ItemProperty -Path HKLM:Software\Microsoft\Windows\CurrentVersion\Policies\System
      //     -Name SoftwareSASGeneration -Value 1
      LOG_DEBUG("calling SendSAS to simulate ctrl+alt+del");
      m_sendSasFunc(FALSE);
    }
  }
}
