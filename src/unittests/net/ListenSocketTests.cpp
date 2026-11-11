/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "../deskflow/MockEventQueue.h"
#include "arch/Arch.h"
#include "arch/ArchException.h"
#include "base/Log.h"
#include "net/IDataSocket.h"
#include "net/NetworkAddress.h"
#include "net/SecureListenSocket.h"
#include "net/SocketException.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPListenSocket.h"

#include <QRandomGenerator>
#include <QSemaphore>
#include <QTest>

#include <atomic>
#include <memory>

namespace {

class FailingAcceptArch : public Arch
{
public:
  bool failNextAccept = false;
  ArchSocket acceptSocket(ArchSocket socket, ArchNetAddress *address) override
  {
#ifdef Q_OS_WIN
    auto *accepted = ArchNetworkWinsock::acceptSocket(socket, address);
#else
    auto *accepted = ArchNetworkBSD::acceptSocket(socket, address);
#endif
    if (failNextAccept) {
      failNextAccept = false;
      // A real accept call reenables Windows FD_ACCEPT notification even when
      // it fails. Consume/drop this connection before injecting the failure so
      // the test preserves that platform side effect rather than inventing a
      // failure before Winsock was ever called.
      if (accepted)
        closeSocket(accepted);
      throw ArchNetworkInterruptedException("injected recoverable accept interruption");
    }
    return accepted;
  }
};

class ListenEvents : public MockEventQueue
{
public:
  QSemaphore incoming;
  void addEvent(Event &&event) override
  {
    if (event.getType() == EventTypes::ListenSocketConnecting)
      incoming.release();
    Event::deleteData(event);
  }
};

} // namespace

class ListenSocketTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase()
  {
#ifdef Q_OS_WIN
    m_arch.init();
#endif
  }

  void recoverableAcceptRearmsListener_data()
  {
    QTest::addColumn<bool>("tls");
    QTest::newRow("tcp") << false;
    QTest::newRow("tls") << true;
  }

  void recoverableAcceptRearmsListener()
  {
    QFETCH(bool, tls);
    ListenEvents events;
    SocketMultiplexer multiplexer;
    std::unique_ptr<TCPListenSocket> listener;
    if (tls)
      listener = std::make_unique<SecureListenSocket>(
          &events, &multiplexer, IArchNetwork::AddressFamily::INet, SecurityLevel::Encrypted
      );
    else
      listener = std::make_unique<TCPListenSocket>(&events, &multiplexer, IArchNetwork::AddressFamily::INet);

    // Isolated loopback listener: never contacts or changes the running Deskflow pair.
    std::unique_ptr<NetworkAddress> address;
    for (int attempt = 0; attempt < 10 && !address; ++attempt) {
      auto candidate = std::make_unique<NetworkAddress>("127.0.0.1", QRandomGenerator::global()->bounded(40000, 60000));
      candidate->resolve();
      try {
        listener->bind(*candidate);
        address = std::move(candidate);
      } catch (const SocketAddressInUseException &) {
      }
    }
    QVERIFY(address);

    const auto closeSocket = [](ArchSocketImpl *socket) { ARCH->closeSocket(socket); };
    std::unique_ptr<ArchSocketImpl, decltype(closeSocket)> peer(
        ARCH->newSocket(IArchNetwork::AddressFamily::INet, IArchNetwork::SocketType::Stream), closeSocket
    );
    ARCH->connectSocket(peer.get(), address->getAddress());
    QVERIFY2(events.incoming.tryAcquire(1, 3000), "initial loopback connection was not announced");
    m_arch.failNextAccept = true;
    QVERIFY(!listener->accept());
    std::unique_ptr<ArchSocketImpl, decltype(closeSocket)> retryPeer(
        ARCH->newSocket(IArchNetwork::AddressFamily::INet, IArchNetwork::SocketType::Stream), closeSocket
    );
    ARCH->connectSocket(retryPeer.get(), address->getAddress());
    QVERIFY2(events.incoming.tryAcquire(1, 3000), "recoverable accept error left listener permanently unpolled");
    if (!tls) {
      auto accepted = listener->accept();
      QVERIFY(accepted);
    }
  }

private:
  FailingAcceptArch m_arch;
  Log m_log;
};

QTEST_GUILESS_MAIN(ListenSocketTests)
#include "ListenSocketTests.moc"
