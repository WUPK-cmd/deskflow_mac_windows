/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerProxyTests.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "client/Client.h"
#include "client/ServerProxy.h"
#include "deskflow/AppUtil.h"
#include "deskflow/Computer.h"
#include "deskflow/PlatformComputer.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "net/ISocketFactory.h"

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class FakeStream : public deskflow::IStream
{
public:
  void push(const std::string &bytes)
  {
    m_chunks.push_back(bytes);
  }

  void close() override
  {
    m_chunks.clear();
    m_inputShutdown = true;
  }

  uint32_t read(void *buffer, uint32_t size) override
  {
    if (m_inputShutdown || m_chunks.empty() || size == 0) {
      return 0;
    }

    auto &front = m_chunks.front();
    const size_t bytesToRead = std::min(static_cast<size_t>(size), front.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, front.data(), bytesToRead);
    }

    front.erase(0, bytesToRead);
    if (front.empty()) {
      m_chunks.pop_front();
    }
    return static_cast<uint32_t>(bytesToRead);
  }

  void write(const void *bytes, uint32_t size) override
  {
    m_written.append(static_cast<const char *>(bytes), size);
  }

  std::string takeWritten()
  {
    return std::exchange(m_written, {});
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
    close();
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<FakeStream *>(this);
  }

  bool isReady() const override
  {
    return !m_inputShutdown && !m_chunks.empty();
  }

  uint32_t getSize() const override
  {
    size_t total = 0;
    for (const auto &chunk : m_chunks) {
      total += chunk.size();
    }
    return static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
  }

private:
  std::deque<std::string> m_chunks;
  std::string m_written;
  bool m_inputShutdown = false;
};

class RecordingEventQueue : public IEventQueue
{
public:
  ~RecordingEventQueue() override
  {
    for (const auto &event : m_addedEvents) {
      Event::deleteData(event);
    }
  }

  int loop() override
  {
    return 0;
  }

  void adoptBuffer(IEventQueueBuffer *) override
  {
  }

  bool getEvent(Event &, double = -1.0) override
  {
    return false;
  }

  bool dispatchEvent(const Event &event) override
  {
    const auto handler = m_handlers.find(HandlerKey{event.getType(), event.getTarget()});
    if (handler == m_handlers.end()) {
      return false;
    }

    const auto callback = handler->second;
    callback(event);
    return true;
  }

  void addEvent(Event &&event) override
  {
    m_addedEvents.emplace_back(std::move(event));
  }

  EventQueueTimer *newTimer(double, void *) override
  {
    return timer();
  }

  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return timer();
  }

  void deleteTimer(EventQueueTimer *) override
  {
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[HandlerKey{type, target}] = handler;
  }

  void removeHandler(EventTypes type, void *target) override
  {
    m_handlers.erase(HandlerKey{type, target});
  }

  void removeHandlers(void *target) override
  {
    for (auto it = m_handlers.begin(); it != m_handlers.end();) {
      if (it->first.target == target) {
        it = m_handlers.erase(it);
      } else {
        ++it;
      }
    }
  }

  void waitForReady() const override
  {
  }

  void *getSystemTarget() override
  {
    return this;
  }

  EventQueueTimer *timer()
  {
    return reinterpret_cast<EventQueueTimer *>(&m_timerStorage);
  }

  const std::vector<Event> &addedEvents() const
  {
    return m_addedEvents;
  }

private:
  struct HandlerKey
  {
    EventTypes type;
    void *target;

    bool operator<(const HandlerKey &other) const
    {
      if (type != other.type) {
        return static_cast<uint32_t>(type) < static_cast<uint32_t>(other.type);
      }
      return std::less<void *>{}(target, other.target);
    }
  };

  int m_timerStorage = 0;
  std::map<HandlerKey, EventHandler> m_handlers;
  std::vector<Event> m_addedEvents;
};

class TestServerProxy : public ServerProxy
{
public:
  using ServerProxy::ServerProxy;

  bool parseHandshakeMessageReturnsDisconnect(const uint8_t *code)
  {
    return parseHandshakeMessage(code) == ConnectionResult::Disconnect;
  }
};

// The receiver is real; only the OS and connection factory are inert. No native
// input, clipboard access, connection, or application event loop is started.
class InertPlatformComputer : public PlatformComputer
{
public:
  explicit InertPlatformComputer(IEventQueue *events) : PlatformComputer(events)
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<InertPlatformComputer *>(this);
  }
  bool getClipboard(ClipboardID, IClipboard *) const override
  {
    return false;
  }
  void getShape(int32_t &, int32_t &, int32_t &, int32_t &) const override
  {
  }
  void getCursorPos(int32_t &, int32_t &) const override
  {
  }
  void reconfigure(uint32_t) override
  {
  }
  uint32_t activeSides() override
  {
    return 0;
  }
  void warpCursor(int32_t, int32_t) override
  {
  }
  uint32_t registerHotKey(KeyID, KeyModifierMask) override
  {
    return 0;
  }
  void unregisterHotKey(uint32_t) override
  {
  }
  void fakeInputBegin() override
  {
  }
  void fakeInputEnd() override
  {
  }
  int32_t getJumpZoneSize() const override
  {
    return 0;
  }
  bool isAnyMouseButtonDown(uint32_t &) const override
  {
    return false;
  }
  void getCursorCenter(int32_t &, int32_t &) const override
  {
  }
  void fakeMouseButton(ButtonID, bool) override
  {
  }
  void fakeMouseMove(int32_t, int32_t) override
  {
  }
  void fakeMouseRelativeMove(int32_t, int32_t) const override
  {
  }
  void fakeMouseWheel(ScrollDelta) const override
  {
  }
  void enable() override
  {
  }
  void disable() override
  {
  }
  void enter() override
  {
  }
  bool canLeave() override
  {
    return true;
  }
  void leave() override
  {
  }
  bool setClipboard(ClipboardID, const IClipboard *) override
  {
    return false;
  }
  void checkClipboards() override
  {
  }
  void openScreensaver(bool) override
  {
  }
  void closeScreensaver() override
  {
  }
  void screensaver(bool) override
  {
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
  void setSequenceNumber(uint32_t) override
  {
  }
  bool isPrimary() const override
  {
    return false;
  }
  std::string getSecureInputApp() const override
  {
    return {};
  }
  void setHalfDuplexMask(KeyModifierMask) override
  {
  }

protected:
  void updateButtons() override
  {
  }
  IKeyState *getKeyState() const override
  {
    return nullptr;
  }
  void handleSystemEvent(const Event &) override
  {
  }
};

class InertSocketFactory : public ISocketFactory
{
public:
  IDataSocket *create(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return nullptr;
  }
  IListenSocket *createListen(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return nullptr;
  }
};

class RecordingClient : public Client
{
public:
  RecordingClient(IEventQueue *events, deskflow::Computer *computer)
      : Client(events, "input-test", NetworkAddress("unused", 24800), new InertSocketFactory, computer)
  {
  }
  void handshakeComplete() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
  void enter(int32_t, int32_t, uint32_t, KeyModifierMask, bool) override
  {
    calls.push_back("enter");
  }
  bool leave() override
  {
    calls.push_back("leave");
    return true;
  }
  void mouseDown(ButtonID id) override
  {
    calls.push_back(QString("down:%1").arg(id));
  }
  void mouseUp(ButtonID id) override
  {
    calls.push_back(QString("up:%1").arg(id));
  }
  void mouseMove(int32_t x, int32_t y) override
  {
    calls.push_back(QString("move:%1,%2").arg(x).arg(y));
  }
  void mouseRelativeMove(int32_t x, int32_t y) override
  {
    calls.push_back(QString("relative:%1,%2").arg(x).arg(y));
  }
  void keyUp(KeyID, KeyModifierMask, KeyButton) override
  {
    calls.push_back("keyUp");
  }
  void mouseWheel(int32_t x, int32_t y) override
  {
    calls.push_back(QString("wheel:%1,%2").arg(x).arg(y));
  }
  QStringList calls;
};

class InputReceiver
{
public:
  InputReceiver()
      : computer(new InertPlatformComputer(&events), &events),
        client(&events, &computer),
        proxy(&client, &stream, &events)
  {
    // The normal options message completes the real protocol handshake.
    OptionsList options;
    send(kMsgDSetOptions, &options);
    pump();
  }

  template <typename... Args> void send(const char *format, Args... args)
  {
    ProtocolUtil::writef(&stream, format, args...);
    stream.push(stream.takeWritten());
  }
  void pump()
  {
    events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget()));
  }

  RecordingEventQueue events;
  FakeStream stream;
  deskflow::Computer computer;
  RecordingClient client;
  ServerProxy proxy;
};

Client *undereferenceableClient()
{
  // These paths must queue cleanup without calling through to Client.
  return reinterpret_cast<Client *>(0x1);
}

TestAppUtil &testAppUtil()
{
  static TestAppUtil util;
  return util;
}

const Client::DisconnectRequest *disconnectRequest(const RecordingEventQueue &events)
{
  if (events.addedEvents().size() != 1) {
    return nullptr;
  }
  return static_cast<const Client::DisconnectRequest *>(events.addedEvents().front().getDataObject());
}

} // namespace

void ServerProxyTests::initTestCase()
{
  QStandardPaths::setTestModeEnabled(true);
  static QTemporaryDir settings;
  QVERIFY(settings.isValid());
  Settings::setSettingsFile(settings.filePath("settings.ini"));
  Settings::setStateFile(settings.filePath("state.ini"));
  (void)testAppUtil();
  m_log.setFilter(LogLevel::Level::Debug);
}

void ServerProxyTests::handleKeepAliveAlarm_timeout_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::Timer, events.timer())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server is not responding"));
}

void ServerProxyTests::handleData_incompleteMessage_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  stream.push("DF");
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("incomplete message from server"));
}

void ServerProxyTests::parseHandshakeMessage_protocolError_queuesRefusalRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  TestServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(proxy.parseHandshakeMessageReturnsDisconnect(reinterpret_cast<const uint8_t *>(kMsgEBad)));
  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Refuse);
  QVERIFY(request->refusalReason() == deskflow::core::ConnectionRefusal::ProtocolError);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server reported a protocol error"));
}

void ServerProxyTests::mouseMove_idleBatch_coalescesToLatest()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseMove, 10, 20);
  receiver.send(kMsgDMouseMove, 30, 40);
  receiver.send(kMsgDMouseMove, 50, 60);
  receiver.pump();
  QCOMPARE(receiver.client.calls, QStringList({"move:50,60"}));
}

void ServerProxyTests::mouseMove_dragBatch_preservesEveryPosition()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseDown, kButtonLeft);
  receiver.send(kMsgDMouseMove, 10, 20);
  receiver.send(kMsgDMouseMove, 30, 40);
  receiver.send(kMsgDMouseMove, 50, 60);
  receiver.send(kMsgDMouseUp, kButtonLeft);
  receiver.pump();
  QCOMPARE(receiver.client.calls, QStringList({"down:1", "move:10,20", "move:30,40", "move:50,60", "up:1"}));
}

void ServerProxyTests::mouseMove_multipleButtons_staysReliableUntilLastRelease()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseDown, kButtonLeft);
  receiver.send(kMsgDMouseDown, kButtonRight);
  receiver.send(kMsgDMouseUp, kButtonLeft);
  receiver.send(kMsgDMouseMove, 10, 20);
  receiver.send(kMsgDMouseMove, 30, 40);
  receiver.send(kMsgDMouseUp, kButtonRight);
  receiver.send(kMsgDMouseMove, 50, 60);
  receiver.send(kMsgDMouseMove, 70, 80);
  receiver.pump();
  QCOMPARE(
      receiver.client.calls, QStringList({"down:1", "down:3", "up:1", "move:10,20", "move:30,40", "up:3", "move:70,80"})
  );
}

void ServerProxyTests::mouseMove_inputBarriers_preserveOrder()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseMove, 10, 20);
  receiver.send(kMsgDMouseMove, 30, 40);
  receiver.send(kMsgDKeyUp, 65, 0, 1);
  receiver.send(kMsgDMouseMove, 50, 60);
  receiver.send(kMsgDMouseWheel, 0, 120);
  receiver.send(kMsgDMouseDown, kButtonLeft);
  receiver.send(kMsgDMouseMove, 70, 80);
  receiver.send(kMsgDMouseMove, 90, 100);
  receiver.send(kMsgDMouseUp, kButtonLeft);
  receiver.pump();
  QCOMPARE(
      receiver.client.calls,
      QStringList({"move:30,40", "keyUp", "move:50,60", "wheel:0,120", "down:1", "move:70,80", "move:90,100", "up:1"})
  );
}

void ServerProxyTests::mouseMove_leaveAndEnter_clearHeldButtons()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseDown, kButtonLeft);
  receiver.send(kMsgCLeave);
  receiver.send(kMsgCEnter, 0, 0, 1, 0);
  receiver.send(kMsgDMouseMove, 10, 20);
  receiver.send(kMsgDMouseMove, 30, 40);
  receiver.pump();
  QCOMPARE(receiver.client.calls, QStringList({"down:1", "leave", "enter", "move:30,40"}));
}

void ServerProxyTests::mouseRelativeMove_batch_accumulatesDeltas()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseRelMove, 10, 20);
  receiver.send(kMsgDMouseRelMove, 30, 40);
  receiver.send(kMsgDMouseRelMove, 50, 60);
  receiver.pump();
  QCOMPARE(receiver.client.calls, QStringList({"relative:90,120"}));
}

void ServerProxyTests::mouseMove_relativeThenAbsolute_flushesRelativeFirst()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseRelMove, 10, 20);
  receiver.send(kMsgDMouseRelMove, 30, 40);
  receiver.send(kMsgDMouseMove, 50, 60);
  receiver.send(kMsgDMouseMove, 70, 80);
  receiver.pump();
  QCOMPARE(receiver.client.calls, QStringList({"relative:40,60", "move:70,80"}));
}

void ServerProxyTests::mouseMove_absoluteThenRelative_flushesAbsoluteFirst()
{
  InputReceiver receiver;
  receiver.send(kMsgDMouseMove, 10, 20);
  receiver.send(kMsgDMouseMove, 30, 40);
  receiver.send(kMsgDMouseRelMove, 50, 60);
  receiver.pump();
  QCOMPARE(receiver.client.calls, QStringList({"move:30,40", "relative:50,60"}));
}

QTEST_MAIN(ServerProxyTests)
