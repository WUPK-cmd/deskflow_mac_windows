// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "OutputSchedulingTests.h"

#include "MockEventQueue.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "io/IStream.h"

#include <QByteArray>
#include <QList>
#include <QTest>
#include <QtEndian>

#include <deque>
#include <map>
#include <string>

namespace {

class RecordingEventQueue : public MockEventQueue
{
public:
  ~RecordingEventQueue() override
  {
    for (const auto &event : posted)
      Event::deleteData(event);
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[target][type] = handler;
  }

  void removeHandler(EventTypes type, void *target) override
  {
    const auto targetHandlers = m_handlers.find(target);
    if (targetHandlers != m_handlers.end())
      targetHandlers->second.erase(type);
  }

  void removeHandlers(void *target) override
  {
    m_handlers.erase(target);
  }

  bool dispatchEvent(const Event &event) override
  {
    const auto targetHandlers = m_handlers.find(event.getTarget());
    if (targetHandlers == m_handlers.end())
      return false;

    auto found = targetHandlers->second.find(event.getType());
    if (found == targetHandlers->second.end())
      found = targetHandlers->second.find(EventTypes::Unknown);
    if (found == targetHandlers->second.end())
      return false;

    // A callback can unregister handlers while handling an event.
    const auto handler = found->second;
    handler(event);
    return true;
  }

  void addEvent(Event &&event) override
  {
    posted.push_back(std::move(event));
  }

  std::deque<Event> posted;

private:
  std::map<void *, std::map<EventTypes, EventHandler>, std::less<void *>> m_handlers;
};

class BufferedStream : public deskflow::IStream
{
public:
  explicit BufferedStream(bool buffered = true) : m_buffered(buffered)
  {
  }

  void close() override
  {
    m_closed = true;
    m_pending = false;
  }

  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }

  void write(const void *data, uint32_t size) override
  {
    ++writeAttempts;
    if (m_closed)
      return;
    bytes.append(static_cast<const char *>(data), size);
    if (size)
      m_pending = m_buffered;
  }

  void flush() override
  {
    m_pending = false;
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
    close();
  }

  void *getEventTarget() const override
  {
    return const_cast<BufferedStream *>(this);
  }

  bool isReady() const override
  {
    return false;
  }

  uint32_t getSize() const override
  {
    return 0;
  }

  // Intentionally also compiles against the old IStream, before this becomes virtual.
  bool hasPendingOutput() const
  {
    return m_pending;
  }

  void makeBusy()
  {
    m_pending = true;
  }

  void completeOutput(RecordingEventQueue &events)
  {
    m_pending = false;
    events.dispatchEvent(Event(EventTypes::StreamOutputFlushed, getEventTarget()));
  }

  void drainOutput(RecordingEventQueue &events)
  {
    for (int i = 0; i < 100 && m_pending; ++i)
      completeOutput(events);
  }

  QList<QByteArray> packets() const
  {
    QList<QByteArray> result;
    qsizetype offset = 0;
    while (bytes.size() - offset >= 4) {
      const auto size = qFromBigEndian<quint32>(bytes.constData() + offset);
      offset += 4;
      if (size > static_cast<quint32>(bytes.size() - offset))
        return {};
      result.append(bytes.mid(offset, size));
      offset += size;
    }
    if (offset != bytes.size())
      return {};
    return result;
  }

  QByteArray bytes;
  int writeAttempts = 0;

private:
  const bool m_buffered;
  bool m_pending = false;
  bool m_closed = false;
};

template <typename... Args> QByteArray message(const char *format, Args... args)
{
  BufferedStream stream(false);
  ProtocolUtil::writef(&stream, format, args...);
  return stream.bytes;
}

QByteArray position(int x, int y)
{
  return message(kMsgDMouseMove, x, y);
}

QByteArray clipboard(uint8_t mark, const std::string &data, ClipboardID id = 0, uint32_t sequence = 7)
{
  return message(kMsgDClipboard, id, sequence, mark, &data);
}

void write(PacketStreamFilter &filter, const QByteArray &packet)
{
  filter.write(packet.constData(), static_cast<uint32_t>(packet.size()));
}

} // namespace

void OutputSchedulingTests::keepsLatestUnsentAbsolutePosition()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();

  write(filter, position(1, 2));
  write(filter, position(3, 4));
  write(filter, position(5, 6));
  QVERIFY(stream.bytes.isEmpty());

  stream.completeOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{position(5, 6)}));
}

void OutputSchedulingTests::sendsOnePacketPerOutputCompletion()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  const auto start = clipboard(ChunkType::DataStart, "3");
  const auto data = clipboard(ChunkType::DataChunk, "ABC");
  const auto end = clipboard(ChunkType::DataEnd, "");

  write(filter, start);
  write(filter, data);
  write(filter, end);
  QCOMPARE(stream.packets(), (QList<QByteArray>{start}));
  stream.completeOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{start, data}));
  stream.completeOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{start, data, end}));
}

void OutputSchedulingTests::prioritizesPositionBeforeTrailingClipboardPackets()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  const auto start = clipboard(ChunkType::DataStart, "3");
  const auto data = clipboard(ChunkType::DataChunk, "ABC");
  const auto end = clipboard(ChunkType::DataEnd, "");

  write(filter, start);
  write(filter, position(1, 2));
  write(filter, data);
  write(filter, position(3, 4));
  write(filter, end);
  stream.drainOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{position(3, 4), start, data, end}));
}

void OutputSchedulingTests::respectsControlBarriers_data()
{
  QTest::addColumn<QByteArray>("barrier");
  QTest::addColumn<bool>("dragging");
  QTest::newRow("button down") << message(kMsgDMouseDown, 1) << true;
  QTest::newRow("button up") << message(kMsgDMouseUp, 1) << false;
  QTest::newRow("legacy key down") << message(kMsgDKeyDown1_0, 0x61, 0) << false;
  QTest::newRow("key down") << message(kMsgDKeyDown1_1, 0x61, 0, 0x1e) << false;
  QTest::newRow("key up") << message(kMsgDKeyUp, 0x61, 0, 0x1e) << false;
  QTest::newRow("key repeat") << message(kMsgDKeyRepeat1_1, 0x61, 0, 2, 0x1e) << false;
  QTest::newRow("wheel") << message(kMsgDMouseWheel, 0, 120) << false;
  QTest::newRow("legacy wheel") << message(kMsgDMouseWheel1_0, 120) << false;
  QTest::newRow("enter") << message(kMsgCEnter, 1, 2, 3, 0) << false;
  QTest::newRow("leave") << message(kMsgCLeave) << false;
  QTest::newRow("relative move") << message(kMsgDMouseRelMove, 4, 5) << false;
  QTest::newRow("clipboard ownership") << message(kMsgCClipboard, 0, 7) << false;
}

void OutputSchedulingTests::respectsControlBarriers()
{
  QFETCH(QByteArray, barrier);
  QFETCH(bool, dragging);
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  const auto start = clipboard(ChunkType::DataStart, "3");
  const auto data = clipboard(ChunkType::DataChunk, "ABC");

  write(filter, start);
  write(filter, position(1, 2));
  write(filter, barrier);
  write(filter, data);
  write(filter, position(3, 4));
  stream.drainOutput(events);
  if (dragging)
    QCOMPARE(stream.packets(), (QList<QByteArray>{position(1, 2), start, barrier, data, position(3, 4)}));
  else
    QCOMPARE(stream.packets(), (QList<QByteArray>{position(1, 2), start, barrier, position(3, 4), data}));
}

void OutputSchedulingTests::preservesClipboardBeforePasteKeys()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  const auto start = clipboard(ChunkType::DataStart, "3");
  const auto data = clipboard(ChunkType::DataChunk, "ABC");
  const auto end = clipboard(ChunkType::DataEnd, "");
  const auto controlDown = message(kMsgDKeyDown1_1, 0xffe3, 2, 0x1d);
  const auto pasteDown = message(kMsgDKeyDown1_1, 0x76, 2, 0x2f);

  write(filter, start);
  write(filter, data);
  write(filter, end);
  write(filter, controlDown);
  write(filter, pasteDown);
  stream.drainOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{start, data, end, controlDown, pasteDown}));
}

void OutputSchedulingTests::preservesDragMovesUntilEveryButtonIsReleased()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  const auto leftDown = message(kMsgDMouseDown, 1);
  const auto rightDown = message(kMsgDMouseDown, 2);
  const auto leftUp = message(kMsgDMouseUp, 1);
  const auto rightUp = message(kMsgDMouseUp, 2);

  const QList<QByteArray> beforeRelease{leftDown, position(1, 1), position(2, 2), rightDown, position(3, 3),
                                        leftUp,   position(4, 4), position(5, 5), rightUp};
  for (const auto &packet : beforeRelease)
    write(filter, packet);
  write(filter, position(6, 6));
  write(filter, position(7, 7));
  stream.drainOutput(events);
  auto expected = beforeRelease;
  expected.append(position(7, 7));
  QCOMPARE(stream.packets(), expected);
}

void OutputSchedulingTests::preservesRelativeMotion()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  const auto first = message(kMsgDMouseRelMove, 4, -2);
  const auto second = message(kMsgDMouseRelMove, -3, 5);
  write(filter, first);
  write(filter, second);
  stream.drainOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{first, second}));
}

void OutputSchedulingTests::ignoresStaleAndRepeatedOutputCompletion()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  write(filter, position(1, 1));
  write(filter, position(2, 2));
  write(filter, position(3, 3));
  events.dispatchEvent(Event(EventTypes::StreamOutputFlushed, stream.getEventTarget()));
  QCOMPARE(stream.packets(), (QList<QByteArray>{position(1, 1)}));
  stream.completeOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{position(1, 1), position(3, 3)}));
  stream.completeOutput(events);
  stream.completeOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{position(1, 1), position(3, 3)}));
}

void OutputSchedulingTests::closeCancelsPendingPackets()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  write(filter, position(1, 2));
  write(filter, clipboard(ChunkType::DataStart, "3"));
  filter.close();
  const auto attempts = stream.writeAttempts;
  stream.completeOutput(events);
  stream.completeOutput(events);
  QVERIFY(stream.bytes.isEmpty());
  QCOMPARE(stream.writeAttempts, attempts);
}

void OutputSchedulingTests::shutdownOutputCancelsPendingPackets()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  write(filter, clipboard(ChunkType::DataStart, "3"));
  write(filter, position(1, 2));
  filter.shutdownOutput();
  const auto attempts = stream.writeAttempts;
  stream.completeOutput(events);
  stream.completeOutput(events);
  QVERIFY(stream.bytes.isEmpty());
  QCOMPARE(stream.writeAttempts, attempts);
}

void OutputSchedulingTests::flushDrainsScheduledPacketsBeforeReturning()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  const auto start = clipboard(ChunkType::DataStart, "3");
  const auto data = clipboard(ChunkType::DataChunk, "ABC");
  const auto end = clipboard(ChunkType::DataEnd, "");
  const auto key = message(kMsgDKeyDown1_1, 0x76, 2, 0x2f);
  write(filter, start);
  write(filter, position(1, 2));
  write(filter, data);
  write(filter, position(3, 4));
  write(filter, end);
  write(filter, key);

  filter.flush();

  const QList<QByteArray> expected{position(3, 4), start, data, end, key};
  QCOMPARE(stream.packets(), expected);
  QVERIFY(!stream.hasPendingOutput());
  stream.completeOutput(events);
  QCOMPARE(stream.packets(), expected);
}

void OutputSchedulingTests::outputFailureCancelsPendingPackets_data()
{
  QTest::addColumn<int>("eventType");
  QTest::newRow("output error") << static_cast<int>(EventTypes::StreamOutputError);
  QTest::newRow("output shutdown") << static_cast<int>(EventTypes::StreamOutputShutdown);
  QTest::newRow("socket disconnected") << static_cast<int>(EventTypes::SocketDisconnected);
}

void OutputSchedulingTests::outputFailureCancelsPendingPackets()
{
  QFETCH(int, eventType);
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  const auto sent = position(1, 2);
  write(filter, sent);
  write(filter, clipboard(ChunkType::DataStart, "3"));
  write(filter, position(3, 4));
  const auto attempts = stream.writeAttempts;

  QVERIFY(events.dispatchEvent(Event(static_cast<EventTypes>(eventType), stream.getEventTarget())));
  stream.completeOutput(events);
  stream.completeOutput(events);
  write(filter, position(5, 6));

  QCOMPARE(stream.packets(), (QList<QByteArray>{sent}));
  QCOMPARE(stream.writeAttempts, attempts);
}

void OutputSchedulingTests::unbufferedOutputPreservesImmediateOrder()
{
  RecordingEventQueue events;
  BufferedStream stream(false);
  PacketStreamFilter filter(&events, &stream, false);
  const QList<QByteArray> expected{position(1, 2), position(3, 4),           message(kMsgDMouseDown, 1),
                                   position(5, 6), message(kMsgDMouseUp, 1), message(kMsgCLeave)};
  for (const auto &packet : expected)
    write(filter, packet);
  QCOMPARE(stream.packets(), expected);
}

void OutputSchedulingTests::streamsHaveIndependentPendingPositions()
{
  RecordingEventQueue events;
  BufferedStream first;
  BufferedStream second;
  PacketStreamFilter firstFilter(&events, &first, false);
  PacketStreamFilter secondFilter(&events, &second, false);
  first.makeBusy();
  second.makeBusy();
  write(firstFilter, position(1, 2));
  write(secondFilter, position(3, 4));
  write(firstFilter, position(5, 6));
  write(secondFilter, position(7, 8));

  first.completeOutput(events);
  QCOMPARE(first.packets(), (QList<QByteArray>{position(5, 6)}));
  QVERIFY(second.bytes.isEmpty());
  second.completeOutput(events);
  QCOMPARE(second.packets(), (QList<QByteArray>{position(7, 8)}));
}

void OutputSchedulingTests::clipboardChunksAreBoundedAndTransfersStaySerialized()
{
  RecordingEventQueue events;
  std::string first(2 * 16 * 1024 + 17, '\0');
  for (size_t i = 0; i < first.size(); ++i)
    first[i] = static_cast<char>(i % 256);
  const std::string second(16 * 1024, 'Z');
  StreamChunker::sendClipboard(first, 0, 7, &events, &events);
  StreamChunker::sendClipboard(second, 1, 8, &events, &events);

  ClipboardID activeId = kClipboardEnd;
  QList<int> completed;
  std::string assembled;
  size_t expectedSize = 0;
  while (!events.posted.empty()) {
    auto event = std::move(events.posted.front());
    events.posted.pop_front();
    QCOMPARE(event.getType(), EventTypes::ClipboardSending);
    BufferedStream stream(false);
    ClipboardChunk::send(&stream, event.getDataObject());
    Event::deleteData(event);
    QVERIFY(stream.bytes.size() >= 14);
    QCOMPARE(stream.bytes.first(4), QByteArray("DCLP"));
    const auto id = static_cast<ClipboardID>(stream.bytes[4]);
    const auto sequence = qFromBigEndian<quint32>(stream.bytes.constData() + 5);
    const auto mark = static_cast<uint8_t>(stream.bytes[9]);
    const auto size = qFromBigEndian<quint32>(stream.bytes.constData() + 10);
    const auto data = stream.bytes.mid(14);
    QCOMPARE(data.size(), static_cast<qsizetype>(size));
    QCOMPARE(sequence, id == 0 ? 7u : 8u);
    if (mark == ChunkType::DataStart) {
      QCOMPARE(activeId, kClipboardEnd);
      activeId = id;
      expectedSize = data.toULongLong();
      assembled.clear();
    } else if (mark == ChunkType::DataChunk) {
      QCOMPARE(id, activeId);
      QVERIFY2(size <= 16 * 1024, "A clipboard data chunk exceeds the 16 KiB scheduling budget");
      assembled.append(data.constData(), size);
    } else {
      QCOMPARE(mark, static_cast<uint8_t>(ChunkType::DataEnd));
      QCOMPARE(id, activeId);
      QCOMPARE(assembled.size(), expectedSize);
      QCOMPARE(assembled, id == 0 ? first : second);
      completed.append(id);
      activeId = kClipboardEnd;
    }
  }
  QCOMPARE(activeId, kClipboardEnd);
  QCOMPARE(completed, (QList<int>{0, 1}));
}

void OutputSchedulingTests::reportsCompletionOnlyWhenAllScheduledOutputIsDrained()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  int completions = 0;
  events.addHandler(EventTypes::StreamOutputFlushed, filter.getEventTarget(), [&](const auto &) { ++completions; });
  write(filter, clipboard(ChunkType::DataStart, "3"));
  write(filter, clipboard(ChunkType::DataChunk, "ABC"));
  write(filter, clipboard(ChunkType::DataEnd, ""));
  stream.completeOutput(events);
  QCOMPARE(completions, 0);
  stream.completeOutput(events);
  QCOMPARE(completions, 0);
  stream.completeOutput(events);
  QCOMPARE(completions, 1);
}

void OutputSchedulingTests::reportsWriteErrorAfterOutputShutdown()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  filter.shutdownOutput();
  write(filter, position(1, 2));
  QCOMPARE(events.posted.size(), size_t(1));
  QVERIFY(events.posted.front().getType() == EventTypes::StreamOutputError);
  QCOMPARE(events.posted.front().getTarget(), filter.getEventTarget());
  QCOMPARE(stream.writeAttempts, 0);
}

void OutputSchedulingTests::coalescesLargeBurstToOneFinalPosition()
{
  RecordingEventQueue events;
  BufferedStream stream;
  PacketStreamFilter filter(&events, &stream, false);
  stream.makeBusy();
  for (int i = 0; i < 10000; ++i)
    write(filter, position(i, i));
  QVERIFY(stream.bytes.isEmpty());
  stream.drainOutput(events);
  QCOMPARE(stream.packets(), (QList<QByteArray>{position(9999, 9999)}));
}

QTEST_MAIN(OutputSchedulingTests)
