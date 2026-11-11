// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QObject>

class OutputLoopbackTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void realTcpStreamPreservesClipboardAndInputOrdering();

private:
  Arch m_arch;
  Log m_log;
};
