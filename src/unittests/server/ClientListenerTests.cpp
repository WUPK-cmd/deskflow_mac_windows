/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientListenerTests.h"

#include "../deskflow/MockEventQueue.h"
#include "common/Settings.h"
#include "deskflow/ProtocolUtil.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/ISocketFactory.h"
#include "server/ClientListener.h"
#include "server/ClientProxy.h"
#include "server/PrimaryClient.h"
#include "server/Server.h"

#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace {

// Dispatches actual handlers, including the Unknown fallback used by stream filters.
// Timer delivery is explicit so event interleavings do not depend on wall-clock sleeps.
class LifecycleEvents : public MockEventQueue
{
public:
  struct Timer
  {
    double duration;
    void *target;
  };
  std::map<std::pair<void *, EventTypes>, EventHandler> handlers;
  std::map<EventQueueTimer *, std::unique_ptr<Timer>> timers;
  std::deque<Event> queued;

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    handlers[{target, type}] = handler;
  }
  void removeHandler(EventTypes type, void *target) override
  {
    handlers.erase({target, type});
  }
  void removeHandlers(void *target) override
  {
    std::erase_if(handlers, [target](const auto &entry) { return entry.first.first == target; });
  }
  bool dispatchEvent(const Event &event) override
  {
    auto it = handlers.find({event.getTarget(), event.getType()});
    if (it == handlers.end())
      it = handlers.find({event.getTarget(), EventTypes::Unknown});
    if (it == handlers.end())
      return false;
    const auto handler = it->second;
    handler(event);
    return true;
  }
  void addEvent(Event &&event) override
  {
    queued.push_back(std::move(event));
  }
  EventQueueTimer *newOneShotTimer(double duration, void *target) override
  {
    auto timer = std::make_unique<Timer>(Timer{duration, target});
    auto *key = reinterpret_cast<EventQueueTimer *>(timer.get());
    if (!target)
      timer->target = key;
    timers.emplace(key, std::move(timer));
    return key;
  }
  void deleteTimer(EventQueueTimer *timer) override
  {
    timers.erase(timer);
  }
  void drain()
  {
    while (!queued.empty()) {
      auto event = std::move(queued.front());
      queued.pop_front();
      dispatchEvent(event);
      Event::deleteData(event);
    }
  }
  void expireTimers()
  {
    std::vector<void *> targets;
    for (const auto &[key, timer] : timers)
      targets.push_back(timer->target);
    for (auto *target : targets)
      dispatchEvent(Event(EventTypes::Timer, target));
    drain();
  }
};

struct SocketState
{
  bool destroyed = false;
  bool handlersRemovedBeforeDestroy = false;
  int flushCalls = 0;
  std::function<void()> onWrite;
};

class LifecycleSocket : public IDataSocket
{
public:
  LifecycleSocket(LifecycleEvents &events, SocketState &state) : IDataSocket(&events), events(events), state(state)
  {
  }
  ~LifecycleSocket() override
  {
    state.handlersRemovedBeforeDestroy =
        std::none_of(events.handlers.begin(), events.handlers.end(), [this](const auto &entry) {
          return entry.first.first == getEventTarget();
        });
    state.destroyed = true;
  }
  void connect(const NetworkAddress &) override
  {
  }
  void bind(const NetworkAddress &) override
  {
  }
  void close() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<LifecycleSocket *>(this);
  }
  bool isFatal() const override
  {
    return false;
  }
  uint32_t read(void *buffer, uint32_t count) override
  {
    count = std::min(count, static_cast<uint32_t>(input.size()));
    if (buffer && count)
      memcpy(buffer, input.constData(), count);
    input.remove(0, count);
    return count;
  }
  void write(const void *, uint32_t) override
  {
    if (state.onWrite)
      state.onWrite();
  }
  void flush() override
  {
    ++state.flushCalls;
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  bool isReady() const override
  {
    return !input.isEmpty();
  }
  uint32_t getSize() const override
  {
    return static_cast<uint32_t>(input.size());
  }
  void receive(const QByteArray &packet)
  {
    const auto size = static_cast<uint32_t>(packet.size());
    input.append(char(size >> 24));
    input.append(char(size >> 16));
    input.append(char(size >> 8));
    input.append(char(size));
    input.append(packet);
    events.dispatchEvent(Event(EventTypes::StreamInputReady, getEventTarget()));
    events.drain();
  }
  LifecycleEvents &events;
  SocketState &state;
  QByteArray input;
};

class LifecycleListenSocket : public IListenSocket
{
public:
  std::unique_ptr<IDataSocket> next;
  void bind(const NetworkAddress &) override
  {
  }
  void close() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<LifecycleListenSocket *>(this);
  }
  std::unique_ptr<IDataSocket> accept() override
  {
    return std::move(next);
  }
};

class LifecycleSocketFactory : public ISocketFactory
{
public:
  LifecycleListenSocket *listen = new LifecycleListenSocket;
  IDataSocket *create(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return nullptr;
  }
  IListenSocket *createListen(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return listen;
  }
};

// No platform computer is created: this primary supplies only the server lifecycle
// operations exercised here, and never captures or injects native input.
class LifecyclePrimary : public PrimaryClient
{
public:
  LifecyclePrimary() : PrimaryClient("primary", nullptr)
  {
  }
  void enable() override
  {
  }
  void disable() override
  {
  }
  void reconfigure(uint32_t) override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<LifecyclePrimary *>(this);
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = y = 0;
  }
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = y = 0;
    w = h = 100;
  }
  KeyModifierMask getToggleMask() const override
  {
    return 0;
  }
  uint32_t registerHotKey(KeyID, KeyModifierMask) override
  {
    return 1;
  }
  void unregisterHotKey(uint32_t) override
  {
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
};

struct ListenerUnderTest
{
  LifecycleEvents events;
  deskflow::server::Config config{&events};
  LifecyclePrimary primary;
  std::unique_ptr<Server> server;
  LifecycleListenSocket *listen;
  std::unique_ptr<ClientListener> listener;

  explicit ListenerUnderTest(SecurityLevel security = SecurityLevel::PlainText)
  {
    config.addComputer("primary");
    config.addComputer("peer");
    // Server stores this required non-null pointer but lifecycle operations use
    // LifecyclePrimary exclusively; it is never dereferenced by these tests.
    server = std::make_unique<Server>(config, &primary, reinterpret_cast<deskflow::Computer *>(&primary), &events);
    auto factory = std::make_unique<LifecycleSocketFactory>();
    listen = factory->listen;
    listener = std::make_unique<ClientListener>(NetworkAddress(24800), std::move(factory), &events, security);
    listener->setServer(server.get());
  }
  LifecycleSocket *accept(SocketState &state)
  {
    auto socket = std::make_unique<LifecycleSocket>(events, state);
    auto *raw = socket.get();
    listen->next = std::move(socket);
    events.dispatchEvent(Event(EventTypes::ListenSocketConnecting, listen));
    events.drain();
    return raw;
  }
  void handshake(LifecycleSocket *socket)
  {
    const auto protocol = QByteArray::fromStdString(server->protocolString());
    socket->receive(protocol + QByteArray::fromHex("0001 0000 00000004") + "peer");
    socket->receive("DINF" + QByteArray::fromHex("0000 0000 0064 0064 0000 0000 0000"));
  }
};

} // namespace

void ClientListenerTests::initTestCase()
{
#ifdef Q_OS_WIN
  m_arch.init();
#endif
  static QTemporaryDir settings;
  Settings::setSettingsFile(settings.filePath("settings.ini"));
  Settings::setStateFile(settings.filePath("state.ini"));
}

void ClientListenerTests::tlsDeadlineStartsBeforeHandshake()
{
  SocketState state;
  ListenerUnderTest test(SecurityLevel::Encrypted);
  auto *socket = test.accept(state);
  QVERIFY(!state.destroyed);
  QCOMPARE(test.events.timers.size(), size_t(1));
  test.events.expireTimers();
  QVERIFY(state.destroyed);
  QVERIFY(state.handlersRemovedBeforeDestroy);
  QVERIFY(!test.events.dispatchEvent(Event(EventTypes::ClientListenerAccepted, socket)));
}

void ClientListenerTests::pendingConnectionsAreBoundedAndCapacityRecovers()
{
  std::vector<SocketState> states(100);
  SocketState reconnect;
  ListenerUnderTest test(SecurityLevel::Encrypted);
  for (auto &state : states)
    test.accept(state);
  const auto countAlive = [&] {
    return std::count_if(states.begin(), states.end(), [](const auto &state) { return !state.destroyed; });
  };
  QVERIFY(countAlive() >= 2);
  QVERIFY(countAlive() <= 16);
  test.events.expireTimers();
  QCOMPARE(countAlive(), 0);
  test.accept(reconnect);
  QVERIFY(!reconnect.destroyed);
  test.events.expireTimers();
  QVERIFY(reconnect.destroyed);
}

void ClientListenerTests::acceptedClientOwnsSocketUntilDisconnect()
{
  SocketState state;
  ListenerUnderTest test;
  auto *socket = test.accept(state);
  test.handshake(socket);
  std::unique_ptr<ClientProxy> client(test.listener->getNextClient());
  QVERIFY(client);
  client.reset();
  QVERIFY(state.destroyed);
  QVERIFY(state.handlersRemovedBeforeDestroy);
  QVERIFY(!test.events.dispatchEvent(Event(EventTypes::ClientListenerAccepted, socket)));
  test.listener.reset(); // ownership must not remain with the listener
}

void ClientListenerTests::failedProtocolHandshakeReleasesSocket()
{
  SocketState state;
  ListenerUnderTest test;
  const auto initialHandlers = test.events.handlers.size();
  auto *socket = test.accept(state);
  // Hello creates a proxy, but peer closes before sending its screen info.
  socket->receive(
      QByteArray::fromStdString(test.server->protocolString()) + QByteArray::fromHex("0001 0000 00000004") + "peer"
  );
  test.events.dispatchEvent(Event(EventTypes::StreamInputShutdown, socket));
  test.events.drain();
  QVERIFY(state.destroyed);
  QVERIFY(state.handlersRemovedBeforeDestroy);
  QVERIFY(!test.listener->getNextClient());
  QCOMPARE(test.events.handlers.size(), initialHandlers);
}

void ClientListenerTests::waitingDisconnectRemovesAllHandlers()
{
  SocketState state;
  ListenerUnderTest test;
  auto *socket = test.accept(state);
  test.handshake(socket);
  // Verify correct owning type before exercising the former invalid downcast.
  // The separate ownership assertion safely fails the unfixed implementation.
  auto *client = test.listener->getNextClient();
  QVERIFY(client);
  const auto target = client->getEventTarget();
  delete client;
  QVERIFY(state.destroyed);
  QVERIFY(!test.events.dispatchEvent(Event(EventTypes::ClientProxyDisconnected, target)));
}

void ClientListenerTests::successFollowedByDisconnectBeforeAdoption()
{
  SocketState state;
  ListenerUnderTest test;
  const auto initialHandlers = test.events.handlers.size();
  auto *socket = test.accept(state);
  test.handshake(socket);
  // A real stream shutdown posts a disconnect while the completed proxy is
  // still in the listener queue, before ServerApp obtains it.
  test.events.dispatchEvent(Event(EventTypes::StreamInputShutdown, socket));
  test.events.drain();
  QVERIFY(state.destroyed);
  QVERIFY(state.handlersRemovedBeforeDestroy);
  QVERIFY(!test.listener->getNextClient());
  QCOMPARE(test.events.handlers.size(), initialHandlers);
}

void ClientListenerTests::closeDeadlineIsArmedBeforeWritingClose()
{
  SocketState state;
  ListenerUnderTest test;
  auto *socket = test.accept(state);
  test.handshake(socket);
  auto *client = test.listener->getNextClient();
  QVERIFY(client);
  test.server->adoptClient(client);
  bool sawClose = false;
  bool deadlinePresent = false;
  state.onWrite = [&] {
    sawClose = true;
    deadlinePresent = std::any_of(test.events.timers.begin(), test.events.timers.end(), [](const auto &entry) {
      return entry.second->duration == 5.0;
    });
  };
  test.server->disconnect();
  QVERIFY(sawClose);
  QVERIFY(deadlinePresent);
  QCOMPARE(state.flushCalls, 0);
  state.onWrite = {};
  test.events.expireTimers();
  QVERIFY(state.destroyed);
}

void ClientListenerTests::repeatedReconnectRestoresHandlersAndTimers()
{
  std::vector<SocketState> states(100);
  ListenerUnderTest test;
  const auto initialHandlers = test.events.handlers.size();
  const auto initialTimers = test.events.timers.size();
  for (auto &state : states) {
    auto *socket = test.accept(state);
    test.handshake(socket);
    auto *client = test.listener->getNextClient();
    QVERIFY(client);
    test.server->adoptClient(client);
    test.events.drain();
    test.events.dispatchEvent(Event(EventTypes::StreamInputShutdown, socket));
    test.events.drain();
    QVERIFY(state.destroyed);
    QVERIFY(state.handlersRemovedBeforeDestroy);
    QCOMPARE(test.events.handlers.size(), initialHandlers);
    QCOMPARE(test.events.timers.size(), initialTimers);
  }
}

void ClientListenerTests::pendingTlsFloodPreservesEstablishedClient()
{
  SocketState connectedState;
  std::vector<SocketState> pending(100);
  ListenerUnderTest test(SecurityLevel::Encrypted);
  auto *socket = test.accept(connectedState);
  test.events.dispatchEvent(Event(EventTypes::ClientListenerAccepted, socket));
  test.handshake(socket);
  auto *client = test.listener->getNextClient();
  QVERIFY(client);
  test.server->adoptClient(client);
  test.events.drain();

  for (auto &state : pending)
    test.accept(state);
  int writes = 0;
  connectedState.onWrite = [&] { ++writes; };
  client->mouseMove(12, 34);
  QVERIFY(writes > 0);
  QVERIFY(!connectedState.destroyed);
  // Expire only the pending TLS deadlines, leaving the established peer alone.
  std::vector<void *> deadlines;
  for (const auto &[key, timer] : test.events.timers) {
    if (timer->duration == 30.0)
      deadlines.push_back(timer->target);
  }
  for (auto *target : deadlines)
    test.events.dispatchEvent(Event(EventTypes::Timer, target));
  test.events.drain();
  QVERIFY(std::all_of(pending.begin(), pending.end(), [](const auto &state) { return state.destroyed; }));
  client->mouseMove(56, 78);
  QVERIFY(writes > 1);
  connectedState.onWrite = {};
  test.events.dispatchEvent(Event(EventTypes::StreamInputShutdown, socket));
  test.events.drain();
  QVERIFY(connectedState.destroyed);
}

QTEST_MAIN(ClientListenerTests)
