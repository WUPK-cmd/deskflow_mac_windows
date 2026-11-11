// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "../deskflow/MockEventQueue.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/Clipboard.h"
#include "deskflow/Computer.h"
#include "deskflow/PlatformComputer.h"
#include "io/IStream.h"
#include "server/ClientProxy1_6.h"
#include "server/PrimaryClient.h"
#include "server/Server.h"

#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>

namespace {
class FlowEvents : public MockEventQueue
{
public:
  struct Timer
  {
    double duration;
    void *target;
  };
  ~FlowEvents() override
  {
    for (const auto &event : queued)
      Event::deleteData(event);
  }
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
  EventQueueTimer *newTimer(double duration, void *target) override
  {
    return newOneShotTimer(duration, target);
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
  bool fireClipboardRetry()
  {
    const auto it =
        std::find_if(timers.begin(), timers.end(), [](const auto &entry) { return entry.second->duration == 0.05; });
    if (it == timers.end())
      return false;
    auto *target = it->second->target;
    dispatchEvent(Event(EventTypes::Timer, target));
    drain();
    return true;
  }
  std::map<std::pair<void *, EventTypes>, EventHandler> handlers;
  std::map<EventQueueTimer *, std::unique_ptr<Timer>> timers;
  std::deque<Event> queued;
};

void setText(Clipboard &clipboard, const std::string &text)
{
  clipboard.open(0);
  clipboard.empty();
  clipboard.add(IClipboard::Format::Text, text);
  clipboard.close();
}

// Only the native boundary is inert. The test uses the production Computer,
// PrimaryClient, Server event handlers and peer clipboard serialization.
class FlowPlatform : public PlatformComputer
{
public:
  explicit FlowPlatform(IEventQueue *events) : PlatformComputer(events)
  {
    setText(contents, "startup-copy");
  }
  bool readable = true;
  mutable int reads = 0;
  Clipboard contents;
  void *getEventTarget() const override
  {
    return const_cast<FlowPlatform *>(this);
  }
  bool getClipboard(ClipboardID, IClipboard *out) const override
  {
    ++reads;
    return readable && Clipboard::copy(out, &contents);
  }
  bool setClipboard(ClipboardID, const IClipboard *value) override
  {
    if (value)
      return Clipboard::copy(&contents, value);
    contents.open(0);
    const auto result = contents.empty();
    contents.close();
    return result;
  }
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = y = 0;
    w = h = 100;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = y = 50;
  }
  void getCursorCenter(int32_t &x, int32_t &y) const override
  {
    x = y = 50;
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
    return 1;
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
    return true;
  }
  std::string getSecureInputApp() const override
  {
    return {};
  }
  void updateKeyMap() override
  {
  }
  void updateKeyState() override
  {
  }
  void setHalfDuplexMask(KeyModifierMask) override
  {
  }
  KeyModifierMask getActiveModifiers() const override
  {
    return 0;
  }
  KeyModifierMask pollActiveModifiers() const override
  {
    return 0;
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

class FlowStream : public deskflow::IStream
{
public:
  std::string output;
  void close() override
  {
  }
  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }
  void write(const void *data, uint32_t size) override
  {
    output.append(static_cast<const char *>(data), size);
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<FlowStream *>(this);
  }
  bool isReady() const override
  {
    return false;
  }
  uint32_t getSize() const override
  {
    return 0;
  }
};

class FlowPeer : public ClientProxy1_6
{
public:
  FlowPeer(const std::string &name, FlowStream *stream, Server *server, IEventQueue *events)
      : ClientProxy1_6(name, stream, server, events)
  {
  }
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = y = 0;
    w = h = 100;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = y = 50;
  }
};

struct ServerFlow
{
  FlowEvents events;
  FlowPlatform *platform = new FlowPlatform(&events);
  deskflow::Computer computer{platform, &events};
  PrimaryClient primary{"primary", &computer};
  deskflow::server::Config config{&events};
  std::unique_ptr<Server> server;

  ServerFlow()
  {
    config.addComputer("primary");
    config.addComputer("peer");
    config.addComputer("fresh-peer");
    server = std::make_unique<Server>(config, &primary, &computer, &events);
    events.drain();
  }
  FlowStream *addPeer(const std::string &name)
  {
    auto *stream = new FlowStream;
    server->adoptClient(new FlowPeer(name, stream, server.get(), &events));
    events.drain();
    stream->output.clear();
    return stream;
  }
  bool switchTo(const std::string &name)
  {
    Server::SwitchToComputerInfo info(name);
    const bool dispatched = events.dispatchEvent(Event(
        EventTypes::ServerSwitchToComputer, config.getInputFilter(), static_cast<void *>(&info),
        Event::EventFlags::DontFreeData
    ));
    events.drain();
    return dispatched;
  }
};

size_t occurrences(const std::string &data, const std::string &needle)
{
  size_t count = 0;
  for (size_t pos = 0; (pos = data.find(needle, pos)) != std::string::npos; pos += needle.size())
    ++count;
  return count;
}
} // namespace

class ServerClipboardFlowTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void startupCaptureFailureDoesNotPublishPlaceholder();
  void refreshCaptureFailureDoesNotPublishCachedData();

private:
  Arch m_arch;
  Log m_log;
};

void ServerClipboardFlowTests::initTestCase()
{
#ifdef Q_OS_WIN
  m_arch.init();
#endif
  static QTemporaryDir settings;
  Settings::setSettingsFile(settings.filePath("settings.ini"));
  Settings::setStateFile(settings.filePath("state.ini"));
}

void ServerClipboardFlowTests::startupCaptureFailureDoesNotPublishPlaceholder()
{
  ServerFlow flow;
  auto *peer = flow.addPeer("peer");
  flow.platform->readable = false;
  QVERIFY(flow.switchTo("peer"));
  QCOMPARE(flow.platform->reads, 2);
  // Check the actual serialized peer output, not merely a setClipboard call.
  QCOMPARE(occurrences(peer->output, "DCLP"), size_t(0));

  setText(flow.platform->contents, "recovered-startup-copy");
  flow.platform->readable = true;
  QVERIFY(flow.events.fireClipboardRetry());
  QCOMPARE(occurrences(peer->output, "DCLP"), size_t(6)); // start, data, end for each logical ID
  QCOMPARE(occurrences(peer->output, "recovered-startup-copy"), size_t(2));
  QVERIFY(!flow.events.fireClipboardRetry());
}

void ServerClipboardFlowTests::refreshCaptureFailureDoesNotPublishCachedData()
{
  ServerFlow flow;
  auto *firstPeer = flow.addPeer("peer");
  setText(flow.platform->contents, "previously-cached-copy");
  QVERIFY(flow.switchTo("peer"));
  QCOMPARE(occurrences(firstPeer->output, "previously-cached-copy"), size_t(2));
  QVERIFY(flow.switchTo("primary"));

  // A newly connected peer is dirty, so it would receive the old server cache
  // if failed refreshes did not set the pending-capture state.
  auto *freshPeer = flow.addPeer("fresh-peer");
  setText(flow.platform->contents, "latest-local-copy");
  flow.platform->readable = false;
  QVERIFY(flow.switchTo("fresh-peer"));
  QCOMPARE(occurrences(freshPeer->output, "DCLP"), size_t(0));
  QCOMPARE(occurrences(freshPeer->output, "previously-cached-copy"), size_t(0));

  flow.platform->readable = true;
  QVERIFY(flow.events.fireClipboardRetry());
  QCOMPARE(occurrences(freshPeer->output, "DCLP"), size_t(6));
  QCOMPARE(occurrences(freshPeer->output, "latest-local-copy"), size_t(2));
  QCOMPARE(occurrences(freshPeer->output, "previously-cached-copy"), size_t(0));
  QVERIFY(!flow.events.fireClipboardRetry());
}

QTEST_GUILESS_MAIN(ServerClipboardFlowTests)
#include "ServerClipboardFlowTests.moc"
