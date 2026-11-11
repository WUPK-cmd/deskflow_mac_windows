// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "MockEventQueue.h"
#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "deskflow/Computer.h"
#include "deskflow/PlatformComputer.h"
#include "server/PrimaryClient.h"

#include <QTest>
#include <map>
#include <vector>

namespace {
class RetryEvents : public MockEventQueue
{
public:
  ~RetryEvents() override
  {
    for (const auto &event : queued)
      Event::deleteData(event);
    while (!timers.empty())
      deleteTimer(timers.begin()->first);
  }
  EventQueueTimer *newOneShotTimer(double, void *target) override
  {
    auto *timer = reinterpret_cast<EventQueueTimer *>(new int(0));
    timers[timer] = target ? target : timer;
    return timer;
  }
  void deleteTimer(EventQueueTimer *timer) override
  {
    if (timers.erase(timer))
      delete reinterpret_cast<int *>(timer);
  }
  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    handlers[{type, target}] = handler;
  }
  void removeHandler(EventTypes type, void *target) override
  {
    handlers.erase({type, target});
  }
  void addEvent(Event &&event) override
  {
    queued.push_back(std::move(event));
  }
  bool fire()
  {
    if (timers.empty())
      return false;
    auto *target = timers.begin()->second;
    auto it = handlers.find({EventTypes::Timer, target});
    if (it == handlers.end())
      return false;
    auto handler = it->second;
    handler(Event(EventTypes::Timer, target));
    return true;
  }
  std::map<EventQueueTimer *, void *> timers;
  std::map<std::pair<EventTypes, void *>, EventHandler> handlers;
  std::vector<Event> queued;
};

std::string getText(const IClipboard &clipboard)
{
  clipboard.open(0);
  auto text = clipboard.get(IClipboard::Format::Text);
  clipboard.close();
  return text;
}
void setText(Clipboard &clipboard, const std::string &text)
{
  clipboard.open(0);
  clipboard.empty();
  clipboard.add(IClipboard::Format::Text, text);
  clipboard.close();
}

// This adapter never touches a native clipboard, hook or device.
class RetryPlatform : public PlatformComputer
{
public:
  explicit RetryPlatform(IEventQueue *events) : PlatformComputer(events)
  {
  }
  bool readable = true;
  bool writable = true;
  mutable int reads = 0;
  uint64_t generation = 1;
  bool sharedNativeClipboard = false;
  // Matches the platform API once available; leaving off override also lets
  // this regression compile against the implementation before the fix.
  ClipboardID nativeClipboardId(ClipboardID id) const override
  {
    return sharedNativeClipboard ? ClipboardID(0) : id;
  }
  uint64_t clipboardSequenceNumber(ClipboardID) const override
  {
    return generation;
  }
  int writes = 0;
  std::vector<std::string> successfulWrites;
  std::string applied;
  void *getEventTarget() const override
  {
    return const_cast<RetryPlatform *>(this);
  }
  bool getClipboard(ClipboardID, IClipboard *out) const override
  {
    ++reads;
    if (!readable)
      return false;
    Clipboard data;
    setText(data, "local-copy");
    return Clipboard::copy(out, &data);
  }
  bool setClipboard(ClipboardID, const IClipboard *value) override
  {
    ++writes;
    if (!writable)
      return false;
    applied = value ? getText(*value) : std::string{};
    successfulWrites.push_back(applied);
    ++generation;
    return true;
  }
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = y = 0;
    w = h = 100;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = y = 0;
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
} // namespace

class ClipboardRetryTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void busyWriteRetriesWithoutAnotherCopy();
  void newestPendingWriteOwnsItsSnapshot();
  void failedReadRetriesAndRenotifies();
  void primaryDoesNotMarkFailedWriteClean();
  void retriesAreBoundedAndDestroyedSafely();
  void successfulWriteDoesNotScheduleRetry();
  void localCopySupersedesDeferredRemoteWrite();
  void aliasedIdsRetainNewestPendingData_data();
  void aliasedIdsRetainNewestPendingData();
  void aliasedIdsRetainNewestOwnershipClear_data();
  void aliasedIdsRetainNewestOwnershipClear();
  void localOwnershipCancelsAllAliasedWrites_data();
  void localOwnershipCancelsAllAliasedWrites();

private:
  Log m_log;
};

void ClipboardRetryTests::busyWriteRetriesWithoutAnotherCopy()
{
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  deskflow::Computer computer(platform, &events);
  Clipboard value;
  setText(value, "remote-copy");
  platform->writable = false;
  computer.setClipboard(0, &value);
  QCOMPARE(platform->writes, 1);
  QCOMPARE(events.timers.size(), size_t(1));
  platform->writable = true;
  QVERIFY(events.fire());
  QCOMPARE(platform->applied, std::string("remote-copy"));
  QVERIFY(events.timers.empty());
}

void ClipboardRetryTests::newestPendingWriteOwnsItsSnapshot()
{
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  deskflow::Computer computer(platform, &events);
  platform->writable = false;
  {
    Clipboard value;
    setText(value, "old");
    computer.setClipboard(0, &value);
    setText(value, "new");
    computer.setClipboard(0, &value);
  }
  QCOMPARE(events.timers.size(), size_t(1));
  platform->writable = true;
  QVERIFY(events.fire());
  QCOMPARE(platform->applied, std::string("new"));
  QVERIFY(events.timers.empty());
}

void ClipboardRetryTests::failedReadRetriesAndRenotifies()
{
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  deskflow::Computer computer(platform, &events);
  computer.setSequenceNumber(42);
  Clipboard out;
  platform->readable = false;
  QVERIFY(!computer.getClipboard(0, &out));
  QCOMPARE(events.timers.size(), size_t(1));
  platform->readable = true;
  QVERIFY(events.fire());
  QCOMPARE(platform->reads, 2);
  QCOMPARE(events.queued.size(), size_t(1));
  QCOMPARE(events.queued.front().getType(), EventTypes::ClipboardGrabbed);
  const auto *info = static_cast<const IComputer::ClipboardInfo *>(events.queued.front().getData());
  QCOMPARE(info->m_id, ClipboardID(0));
  QCOMPARE(info->m_sequenceNumber, uint32_t(42));
  QVERIFY(events.timers.empty());
}

void ClipboardRetryTests::primaryDoesNotMarkFailedWriteClean()
{
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  deskflow::Computer computer(platform, &events);
  PrimaryClient client("local", &computer);
  Clipboard value;
  setText(value, "remote-copy");
  platform->writable = false;
  client.setClipboardDirty(0, true);
  client.setClipboard(0, &value);
  platform->writable = true;
  client.setClipboard(0, &value);
  QCOMPARE(platform->writes, 2);
  QCOMPARE(platform->applied, std::string("remote-copy"));
}

void ClipboardRetryTests::retriesAreBoundedAndDestroyedSafely()
{
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  {
    deskflow::Computer computer(platform, &events);
    Clipboard value;
    setText(value, "pending");
    platform->writable = false;
    computer.setClipboard(0, &value);
    QCOMPARE(events.timers.size(), size_t(1));
    for (int i = 0; i < 50 && events.fire(); ++i) {
    }
    QVERIFY(platform->writes > 1);
    QVERIFY(platform->writes <= 41);
    QVERIFY(events.timers.empty());
    computer.setClipboard(0, &value);
    QCOMPARE(events.timers.size(), size_t(1));
  }
  QVERIFY(events.timers.empty());
  QVERIFY(events.handlers.empty());
}

void ClipboardRetryTests::successfulWriteDoesNotScheduleRetry()
{
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  deskflow::Computer computer(platform, &events);
  Clipboard value;
  setText(value, "remote-copy");
  computer.setClipboard(0, &value);
  QCOMPARE(platform->writes, 1);
  QCOMPARE(platform->applied, std::string("remote-copy"));
  QVERIFY(events.timers.empty());
}

void ClipboardRetryTests::localCopySupersedesDeferredRemoteWrite()
{
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  deskflow::Computer computer(platform, &events);
  Clipboard value;
  setText(value, "stale-remote-copy");
  platform->writable = false;
  computer.setClipboard(0, &value);
  ++platform->generation;
  platform->applied = "new-local-copy";
  platform->writable = true;
  QVERIFY(events.fire());
  QCOMPARE(platform->writes, 1);
  QCOMPARE(platform->applied, std::string("new-local-copy"));
  QVERIFY(events.timers.empty());
}

void ClipboardRetryTests::aliasedIdsRetainNewestPendingData_data()
{
  QTest::addColumn<int>("olderId");
  QTest::addColumn<int>("newerId");
  QTest::newRow("clear-id0-then-data-id1") << 0 << 1;
  QTest::newRow("clear-id1-then-data-id0") << 1 << 0;
}

void ClipboardRetryTests::aliasedIdsRetainNewestPendingData()
{
  QFETCH(int, olderId);
  QFETCH(int, newerId);
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  platform->sharedNativeClipboard = true;
  deskflow::Computer computer(platform, &events);
  Clipboard value;
  setText(value, "newest-remote-data");
  platform->writable = false;
  computer.grabClipboard(static_cast<ClipboardID>(olderId));
  QVERIFY(!computer.setClipboard(static_cast<ClipboardID>(newerId), &value));
  QCOMPARE(platform->writes, 2);
  platform->writable = true;
  QVERIFY(events.fire());
  QCOMPARE(platform->applied, std::string("newest-remote-data"));
  QCOMPARE(platform->successfulWrites.size(), size_t(1));
  QCOMPARE(platform->writes, 3);
  QVERIFY(events.timers.empty());
}

void ClipboardRetryTests::aliasedIdsRetainNewestOwnershipClear_data()
{
  QTest::addColumn<int>("olderId");
  QTest::addColumn<int>("newerId");
  QTest::newRow("data-id0-then-clear-id1") << 0 << 1;
  QTest::newRow("data-id1-then-clear-id0") << 1 << 0;
}

void ClipboardRetryTests::aliasedIdsRetainNewestOwnershipClear()
{
  QFETCH(int, olderId);
  QFETCH(int, newerId);
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  platform->sharedNativeClipboard = true;
  deskflow::Computer computer(platform, &events);
  Clipboard value;
  setText(value, "superseded-remote-data");
  platform->writable = false;
  QVERIFY(!computer.setClipboard(static_cast<ClipboardID>(olderId), &value));
  computer.grabClipboard(static_cast<ClipboardID>(newerId));
  platform->writable = true;
  QVERIFY(events.fire());
  QCOMPARE(platform->applied, std::string{});
  QCOMPARE(platform->successfulWrites.size(), size_t(1));
  QCOMPARE(platform->writes, 3);
  QVERIFY(events.timers.empty());
}

void ClipboardRetryTests::localOwnershipCancelsAllAliasedWrites_data()
{
  QTest::addColumn<int>("notificationId");
  QTest::newRow("notification-id0") << 0;
  QTest::newRow("notification-id1") << 1;
}

void ClipboardRetryTests::localOwnershipCancelsAllAliasedWrites()
{
  QFETCH(int, notificationId);
  RetryEvents events;
  auto *platform = new RetryPlatform(&events);
  platform->sharedNativeClipboard = true;
  deskflow::Computer computer(platform, &events);
  Clipboard value;
  setText(value, "superseded-remote-data");
  platform->writable = false;
  QVERIFY(!computer.setClipboard(0, &value));
  QVERIFY(!computer.setClipboard(1, &value));
  ++platform->generation;
  platform->applied = "new-local-copy";
  computer.cancelClipboardRetries(static_cast<ClipboardID>(notificationId));
  QVERIFY(events.timers.empty());
  platform->writable = true;
  QVERIFY(!events.fire());
  QCOMPARE(platform->writes, 2);
  QCOMPARE(platform->applied, std::string("new-local-copy"));
  QVERIFY(platform->successfulWrites.empty());
}

QTEST_GUILESS_MAIN(ClipboardRetryTests)
#include "ClipboardRetryTests.moc"
