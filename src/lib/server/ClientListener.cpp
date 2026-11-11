/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientListener.h"
#include "server/Server.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/PacketStreamFilter.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/ISocketFactory.h"
#include "net/SecureSocket.h"
#include "net/SocketException.h"
#include "server/ClientProxy.h"
#include "server/ClientProxyUnknown.h"

//
// ClientListener
//

ClientListener::ClientListener(
    const NetworkAddress &address, std::unique_ptr<ISocketFactory> socketFactory, IEventQueue *events,
    SecurityLevel securityLevel
)
    : m_socketFactory{std::move(socketFactory)},
      m_events(events),
      m_securityLevel(securityLevel),
      m_address(address)
{
  assert(m_socketFactory != nullptr);

  try {
    start();
  } catch (SocketAddressInUseException &) {
    cleanupListenSocket();
    m_socketFactory.reset();
    throw;
  } catch (BaseException &) {
    cleanupListenSocket();
    m_socketFactory.reset();
    throw;
  }
  LOG_VERBOSE("listening for clients");
}

ClientListener::~ClientListener()
{
  stop();
  m_socketFactory.reset();
}

void ClientListener::setServer(Server *server)
{
  assert(server != nullptr);
  m_server = server;
}

ClientProxy *ClientListener::getNextClient()
{
  ClientProxy *client = nullptr;
  if (!m_waitingClients.empty()) {
    client = m_waitingClients.front();
    m_waitingClients.pop_front();
    m_events->removeHandler(EventTypes::ClientProxyDisconnected, client);
  }
  return client;
}

void ClientListener::start()
{
  m_listen = m_socketFactory->createListen(ARCH->getAddrFamily(m_address.getAddress()), m_securityLevel);

  // setup event handler
  m_events->addHandler(EventTypes::ListenSocketConnecting, m_listen, [this](const auto &) {
    handleClientConnecting();
  });

  // bind listen address
  LOG_VERBOSE("binding listen socket");
  m_listen->bind(m_address);
}

void ClientListener::stop()
{
  using enum EventTypes;
  LOG_VERBOSE("stop listening for clients");

  // discard already connected clients
  while (!m_newClients.empty())
    removeUnknownClient(*m_newClients.begin());

  // discard waiting clients
  ClientProxy *client = getNextClient();
  while (client != nullptr) {
    delete client;
    client = getNextClient();
  }

  m_events->removeHandler(ListenSocketConnecting, m_listen);
  m_events->removeHandlers(this);
  cleanupListenSocket();
  cleanupClientSockets();
}

void ClientListener::removeUnknownClient(ClientProxyUnknown *unknownClient)
{
  if (unknownClient) {
    m_events->removeHandler(EventTypes::ClientProxyUnknownSuccess, unknownClient);
    m_events->removeHandler(EventTypes::ClientProxyUnknownFailure, unknownClient);
    m_newClients.erase(unknownClient);
    delete unknownClient;
  }
}

void ClientListener::handleClientConnecting()
{
  // accept client connection
  auto socket = m_listen->accept();

  if (!socket)
    return;

  // Leave ample space below Windows' 64 wait-event limit for the listener,
  // established peers and wake events. A two-host reconnect can overlap safely.
  constexpr size_t maxPendingClients = 16;
  if (m_clientSockets.size() + m_newClients.size() + m_waitingClients.size() >= maxPendingClients) {
    LOG_WARN("too many clients waiting for handshake");
    return;
  }

  auto *rawSocketPointer = socket.get();
  auto *timer = m_events->newOneShotTimer(30.0, nullptr);
  m_clientSockets.emplace(rawSocketPointer, PendingSocket{std::move(socket), timer});
  m_events->addHandler(EventTypes::Timer, timer, [this, rawSocketPointer](const auto &) {
    LOG_INFO("new client TLS handshake timed out");
    removeClientSocket(rawSocketPointer);
  });

  m_events->addHandler(
      EventTypes::ClientListenerAccepted, rawSocketPointer->getEventTarget(),
      [this, rawSocketPointer](const auto &) { handleClientAccepted(rawSocketPointer); }
  );

  m_events->addHandler(
      EventTypes::ClientListenerDisconnectedOnAccept, rawSocketPointer->getEventTarget(),
      [this, rawSocketPointer](const auto &) {
        LOG_DEBUG("disconnected client before accept");
        removeClientSocket(rawSocketPointer);
      }
  );

  // When using non SSL, server accepts clients immediately, while SSL
  // has to call secure accept which may require retry
  if (m_securityLevel == SecurityLevel::PlainText) {
    m_events->addEvent(Event(EventTypes::ClientListenerAccepted, rawSocketPointer->getEventTarget()));
  }
}

void ClientListener::handleClientAccepted(IDataSocket *socket)
{
  auto pending = m_clientSockets.find(socket);
  if (pending == m_clientSockets.end())
    return;

  // Transfer the sole owner from the listener to the stream/proxy chain. Remove
  // every accept callback before StreamFilter replaces the socket handlers.
  removeAcceptHandlers(socket);
  auto ownedSocket = std::move(pending->second.socket);
  m_clientSockets.erase(pending);

  LOG_INFO("accepted client connection");

  // filter socket messages, including a packetizing filter
  auto stream = std::make_unique<PacketStreamFilter>(m_events, ownedSocket.get(), true);
  ownedSocket.release();
  assert(m_server != nullptr);

  // create proxy for unknown client
  QString fingerprint;
  if (m_securityLevel == SecurityLevel::PeerAuth) {
    if (auto *secureSocket = dynamic_cast<SecureSocket *>(socket))
      fingerprint = secureSocket->verifiedPeerFingerprint();
  }
  auto *client = new ClientProxyUnknown(stream.get(), 30.0, m_server, m_events, std::move(fingerprint));
  stream.release();

  m_newClients.insert(client);

  // watch for events from unknown client
  m_events->addHandler(EventTypes::ClientProxyUnknownSuccess, client, [this, client](const auto &) {
    handleUnknownClient(client);
  });
  m_events->addHandler(EventTypes::ClientProxyUnknownFailure, client, [this, client](const auto &) {
    removeUnknownClient(client);
  });
}

void ClientListener::handleUnknownClient(ClientProxyUnknown *unknownClient)
{
  // we should have the client in our new client list
  assert(m_newClients.count(unknownClient) == 1);

  // get the real client proxy and install it
  if (auto client = unknownClient->orphanClientProxy(); client) {
    // handshake was successful
    m_waitingClients.push_back(client);
    m_events->addEvent(Event(EventTypes::ClientListenerAccepted, this));

    // watch for client to disconnect while it's in our queue
    m_events->addHandler(EventTypes::ClientProxyDisconnected, client, [this, client](const auto &) {
      handleClientDisconnected(client);
    });
  } else {
    auto *stream = unknownClient->getStream();
    if (stream) {
      stream->close();
    }
  }

  // now finished with unknown client
  removeUnknownClient(unknownClient);
}

void ClientListener::handleClientDisconnected(ClientProxy *client)
{
  // find client in waiting clients queue
  for (WaitingClients::iterator i = m_waitingClients.begin(), n = m_waitingClients.end(); i != n; ++i) {
    if (*i == client) {
      m_waitingClients.erase(i);
      m_events->removeHandler(EventTypes::ClientProxyDisconnected, client);

      // The proxy owns its filter, which owns the actual socket.
      delete client;

      break;
    }
  }
}

void ClientListener::removeClientSocket(IDataSocket *socket)
{
  if (!m_clientSockets.contains(socket))
    return;
  removeAcceptHandlers(socket);
  // Erasing the sole owner destroys the socket only after its handlers and
  // deadline are removed, so queued success/failure events cannot call us.
  m_clientSockets.erase(socket);
}

void ClientListener::removeAcceptHandlers(IDataSocket *socket)
{
  const auto pending = m_clientSockets.find(socket);
  if (pending == m_clientSockets.end())
    return;
  m_events->removeHandler(EventTypes::Timer, pending->second.timer);
  m_events->deleteTimer(pending->second.timer);
  m_events->removeHandlers(socket->getEventTarget());
}

void ClientListener::cleanupListenSocket()
{
  delete m_listen;
  m_listen = nullptr;
}

void ClientListener::cleanupClientSockets()
{
  while (!m_clientSockets.empty())
    removeClientSocket(m_clientSockets.begin()->first);
}
