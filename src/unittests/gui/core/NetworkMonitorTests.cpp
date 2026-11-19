/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NetworkMonitorTests.h"

#include "gui/core/NetworkMonitor.h"

#include <QNetworkInterface>
#include <QSignalSpy>

using namespace deskflow::gui;
void NetworkMonitorTests::testVirtualInterface()
{
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vboxnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vboXnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vmnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("docker-bridge")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("virbr0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("veth0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("br-0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("tun0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("utun0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("awdl0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("p2p0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("llw0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("anpi5")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("tap1")));

  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("eth0")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("enp0s0f")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("wifi")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("wlan0")));
}

void NetworkMonitorTests::matchesActualLoopbackInterface()
{
  const QHostAddress local(QHostAddress::LocalHost);
  QStringList matches;
  for (const auto &interface : QNetworkInterface::allInterfaces()) {
    if (!(interface.flags() & QNetworkInterface::IsUp))
      continue;
    for (const auto &entry : interface.addressEntries()) {
      if (entry.ip() == local) {
        matches.append(interface.humanReadableName());
        break;
      }
    }
  }
  if (matches.size() != 1)
    QSKIP("This host does not have a unique IPv4 loopback interface");
  QCOMPARE(NetworkMonitor::interfaceForAddress(local), matches.first());
  QCOMPARE(NetworkMonitor::interfaceForAddress(QHostAddress("::ffff:127.0.0.1")), matches.first());
  QVERIFY(NetworkMonitor::interfaceForAddress(QHostAddress()).isEmpty());
}

QTEST_MAIN(NetworkMonitorTests)
