/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileLogOutputterTests.h"

#include <QFile>
#include <QFileInfo>
#include <QTest>

void FileLogOutputterTests::init()
{
  QVERIFY(m_dir.isValid());
  m_fileName = m_dir.filePath(QStringLiteral("deskflow.log"));
  QFile::remove(m_fileName);
  QFile::remove(rotatedFileName());
}

QString FileLogOutputterTests::rotatedFileName() const
{
  return QStringLiteral("%1.1").arg(m_fileName);
}

QString FileLogOutputterTests::readAll(const QString &fileName)
{
  QFile file(fileName);
  if (!file.open(QFile::ReadOnly))
    return {};
  return QString::fromUtf8(file.readAll());
}

void FileLogOutputterTests::growLogPastSizeLimit(FileLogOutputter &outputter, const QString &marker)
{
  const QString padding(s_paddingSize, QLatin1Char('x'));

  QVERIFY(outputter.write(LogLevel::Level::Info, marker));

  // rotating renames the live log away, so its absence is what ends the loop
  for (int i = 0; i < s_maxPaddingWrites && QFileInfo::exists(m_fileName); ++i)
    QVERIFY(outputter.write(LogLevel::Level::Info, padding));
}

void FileLogOutputterTests::write_multipleMessages_appendsEachLine()
{
  FileLogOutputter outputter(m_fileName);

  QVERIFY(outputter.write(LogLevel::Level::Info, QStringLiteral("first")));
  QVERIFY(outputter.write(LogLevel::Level::Info, QStringLiteral("second")));

  QCOMPARE(readAll(m_fileName), QStringLiteral("first\nsecond\n"));
}

void FileLogOutputterTests::write_sizeLimitExceeded_preservesPreviousLog()
{
  FileLogOutputter outputter(m_fileName);

  growLogPastSizeLimit(outputter, QStringLiteral("oldest entry"));

  QVERIFY2(QFileInfo::exists(rotatedFileName()), "log was discarded instead of rotated");
  QVERIFY(readAll(rotatedFileName()).startsWith(QStringLiteral("oldest entry\n")));

  QVERIFY(outputter.write(LogLevel::Level::Info, QStringLiteral("after rotating")));
  QCOMPARE(readAll(m_fileName), QStringLiteral("after rotating\n"));
}

void FileLogOutputterTests::write_rotatedLogExists_replacesIt()
{
  FileLogOutputter outputter(m_fileName);

  growLogPastSizeLimit(outputter, QStringLiteral("first batch"));
  QVERIFY(readAll(rotatedFileName()).startsWith(QStringLiteral("first batch\n")));

  growLogPastSizeLimit(outputter, QStringLiteral("second batch"));
  QVERIFY(readAll(rotatedFileName()).startsWith(QStringLiteral("second batch\n")));
}

void FileLogOutputterTests::write_pointerOnlyRejectsOrdinaryMessages()
{
  FileLogOutputter outputter(m_fileName, true);
  const auto valid = QStringLiteral(
      "[2026-11-10T17:00:00.000] INFO: DF_POINTER_METRICS v=1 monotonic_us=123 cumulative=1 "
      "win_warp_us={n:1,sum:250,max:250,hist:1,0,0,0,0,0,0,0,0,0,0}"
  );
  QVERIFY(outputter.write(LogLevel::Level::Info, QStringLiteral("ordinary opaque message")));
  QVERIFY(outputter.write(LogLevel::Level::Warning, QStringLiteral("input-related warning")));
  QVERIFY(outputter.write(LogLevel::Level::Info, valid + QStringLiteral(" unexpected text")));
  QVERIFY(readAll(m_fileName).isEmpty());
  QVERIFY(outputter.write(LogLevel::Level::Info, valid));
  QCOMPARE(readAll(m_fileName), valid + QLatin1Char('\n'));
}

QTEST_MAIN(FileLogOutputterTests)
