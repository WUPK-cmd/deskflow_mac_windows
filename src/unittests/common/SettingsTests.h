/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Settings.h"

#include <QTemporaryDir>
#include <QTest>

class SettingsTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  // Test are run in order top to bottom
  void setSettingsFile();
  void setStateFile();
  void settingsFile();
  void settingsPath();
  void tlsDir();
  void tlsTrustedServersDb();
  void tlsTrustedClientsDb();
  void checkValidSettings();
  void checkCleanComputerName();
  void checkCleanComputerName_LongName();
  void checkClearState();

private:
  QTemporaryDir m_tempSettings;
  const QString m_settingsPathTemp = m_tempSettings.path();
  const QString m_settingsFile = m_tempSettings.filePath("Deskflow.conf");
  const QString m_stateFile = m_tempSettings.filePath("Deskflow.state");

  QString m_settingsPath;
  QString m_expectedTlsDir;
  QString m_expectedTlsServerDB;
  QString m_expectedTlsClientDB;
};
