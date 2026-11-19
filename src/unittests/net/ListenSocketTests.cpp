/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "../deskflow/MockEventQueue.h"
#include "arch/Arch.h"
#include "arch/ArchException.h"
#include "base/Log.h"
#include "io/StreamFilter.h"
#include "net/IDataSocket.h"
#include "net/NetworkAddress.h"
#include "net/SecureListenSocket.h"
#include "net/SocketException.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPListenSocket.h"
#include "net/TCPSocket.h"

#include <QRandomGenerator>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
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
  QSemaphore connected;
  void addEvent(Event &&event) override
  {
    if (event.getType() == EventTypes::ListenSocketConnecting)
      incoming.release();
    if (event.getType() == EventTypes::DataSocketConnected)
      connected.release();
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

  void connectedSocketReportsActualAddresses_data()
  {
    QTest::addColumn<bool>("ipv6");
    QTest::newRow("IPv4") << false;
    QTest::newRow("IPv6") << true;
  }

  void connectedSocketReportsActualAddresses()
  {
    QFETCH(bool, ipv6);
    const auto host = QHostAddress(ipv6 ? QHostAddress::LocalHostIPv6 : QHostAddress::LocalHost);
    QTcpServer server;
    if (!server.listen(host, 0) && ipv6)
      QSKIP("IPv6 loopback is unavailable");
    QVERIFY2(server.isListening(), qPrintable(server.errorString()));

    ListenEvents events;
    SocketMultiplexer multiplexer;
    TCPSocket socket(
        &events, &multiplexer, ipv6 ? IArchNetwork::AddressFamily::INet6 : IArchNetwork::AddressFamily::INet
    );
    StreamFilter filter(&events, &socket, false);
    QCOMPARE(TCPSocket::connectionAddresses(&filter), (QPair<QString, QString>{}));

    NetworkAddress destination(host.toString().toStdString(), server.serverPort());
    destination.resolve();
    socket.connect(destination);
    QVERIFY2(server.waitForNewConnection(3000), qPrintable(server.errorString()));
    QVERIFY2(events.connected.tryAcquire(1, 3000), "Loopback socket did not report a completed connection");
    const auto *peer = server.nextPendingConnection();
    QVERIFY(peer);
    QCOMPARE(TCPSocket::connectionAddresses(&filter), qMakePair(peer->peerAddress().toString(), host.toString()));

    socket.close();
    QCOMPARE(TCPSocket::connectionAddresses(&filter), (QPair<QString, QString>{}));
  }

private:
  FailingAcceptArch m_arch;
  Log m_log;
};

QTEST_GUILESS_MAIN(ListenSocketTests)
#include "ListenSocketTests.moc"
