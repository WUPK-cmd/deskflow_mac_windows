/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

class VersionChecker : public QObject
{
  Q_OBJECT
public:
  explicit VersionChecker(QObject *parent = nullptr);
  void checkLatest() const;
public Q_SLOTS:
  void replyFinished(QNetworkReply *reply);
Q_SIGNALS:
  void updateFound(const QString &version);

private:
  friend class VersionCheckerTests;
  static int compareVersions(const QString &left, const QString &right);

  QNetworkAccessManager *m_network = nullptr;
};
