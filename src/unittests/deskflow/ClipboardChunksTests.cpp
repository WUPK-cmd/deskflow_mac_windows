/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardChunksTests.h"
#include "MockEventQueue.h"

#include "deskflow/ClipboardChunk.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"

#include <algorithm>
#include <cstring>
#include <deque>

namespace {

class PacketEventQueue : public MockEventQueue
{
public:
  void addEvent(Event &&event) override
  {
    posted.append(event.getType());
  }

  QList<EventTypes> posted;
};

class TestPacketStreamFilter : public PacketStreamFilter
{
public:
  using PacketStreamFilter::filterEvent;
  using PacketStreamFilter::PacketStreamFilter;
};

class MemoryStream : public deskflow::IStream
{
public:
  void push(const std::string &bytes)
  {
    m_queue.push_back(bytes);
  }

  void close() override
  {
    m_queue.clear();
    m_inputShutdown = true;
  }

  uint32_t read(void *buffer, uint32_t n) override
  {
    if (m_inputShutdown || m_queue.empty() || n == 0) {
      return 0;
    }

    auto &front = m_queue.front();
    const size_t take = std::min(static_cast<size_t>(n), front.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, front.data(), take);
    }

    front.erase(0, take);
    if (front.empty()) {
      m_queue.pop_front();
    }

    return static_cast<uint32_t>(take);
  }

  void write(const void *, uint32_t) override
  {
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
    return const_cast<MemoryStream *>(this);
  }

  bool isReady() const override
  {
    return !m_inputShutdown && !m_queue.empty();
  }

  uint32_t getSize() const override
  {
    size_t total = 0;
    for (const auto &chunk : m_queue) {
      total += chunk.size();
    }
    return static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
  }

private:
  std::deque<std::string> m_queue;
  bool m_inputShutdown = false;
};

class BufferWriteStream : public deskflow::IStream
{
public:
  const std::string &str() const
  {
    return m_buffer;
  }

  int writes() const
  {
    return m_writes;
  }

  void close() override
  {
    m_outputShutdown = true;
  }

  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }

  void write(const void *buffer, uint32_t n) override
  {
    ++m_writes;
    if (!m_outputShutdown && n != 0) {
      m_buffer.append(static_cast<const char *>(buffer), n);
    }
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
    m_outputShutdown = true;
  }

  void *getEventTarget() const override
  {
    return const_cast<BufferWriteStream *>(this);
  }

  bool isReady() const override
  {
    return false;
  }

  uint32_t getSize() const override
  {
    return 0;
  }

private:
  std::string m_buffer;
  int m_writes = 0;
  bool m_outputShutdown = false;
};

std::string encodeClipboardMsg(ClipboardID id, uint32_t seq, uint8_t mark, const std::string &data)
{
  BufferWriteStream stream;
  auto payload = data;
  ProtocolUtil::writef(&stream, kMsgDClipboard + 4, id, seq, mark, &payload);
  return stream.str();
}

} // namespace

void ClipboardChunksTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Debug);
}

void ClipboardChunksTests::readVariableStringRejectsInvalidLength_data()
{
  QTest::addColumn<QByteArray>("packet");
  QTest::addColumn<bool>("overLimit");
  QTest::newRow("over protocol limit") << QByteArray::fromHex("ffffffff") << true;
  QTest::newRow("length truncated") << QByteArray::fromHex("000000") << false;
  QTest::newRow("payload truncated") << QByteArray::fromHex("0000000278") << false;
}

void ClipboardChunksTests::readVariableStringRejectsInvalidLength()
{
  QFETCH(QByteArray, packet);
  QFETCH(bool, overLimit);
  MemoryStream stream;
  stream.push(packet.toStdString());
  std::string value;
  if (overLimit) {
    QVERIFY_THROWS_EXCEPTION(BadClientException, ProtocolUtil::readf(&stream, "%s", &value));
  } else {
    QVERIFY(!ProtocolUtil::readf(&stream, "%s", &value));
  }
  QVERIFY(value.empty());
}

void ClipboardChunksTests::startFormatData()
{
  ClipboardID id = 0;
  uint32_t sequence = 0;
  std::string mockDataSize("10");
  ClipboardChunk *chunk = ClipboardChunk::start(id, sequence, mockDataSize);
  uint32_t temp_m_chunk;
  memcpy(&temp_m_chunk, &(chunk->m_chunk[1]), 4);

  QCOMPARE(chunk->m_chunk[0], id);
  QCOMPARE(temp_m_chunk, sequence);
  QCOMPARE(chunk->m_chunk[5], ChunkType::DataStart);
  QCOMPARE(chunk->m_chunk[6], '1');
  QCOMPARE(chunk->m_chunk[7], '0');
  QCOMPARE(chunk->m_chunk[8], '\0');
  delete chunk;
}

void ClipboardChunksTests::packetFilterRejectsInvalidHeader_data()
{
  QTest::addColumn<QByteArray>("header");
  QTest::newRow("zero length") << QByteArray::fromHex("00000000");
  QTest::newRow("over limit") << QByteArray::fromHex("ffffffff");
}

void ClipboardChunksTests::packetFilterRejectsInvalidHeader()
{
  QFETCH(QByteArray, header);
  PacketEventQueue events;
  MemoryStream stream;
  TestPacketStreamFilter filter(&events, &stream, false);
  stream.push(header.toStdString());
  filter.filterEvent(Event(EventTypes::StreamInputReady, &stream));
  stream.push(QByteArray::fromHex("0000000178").toStdString());
  filter.filterEvent(Event(EventTypes::StreamInputReady, &stream));

  QVERIFY(!filter.isReady());
  QCOMPARE(events.posted, QList<EventTypes>{EventTypes::StreamInputFormatError});
}

void ClipboardChunksTests::packetFilterRejectsTruncatedTail()
{
  PacketEventQueue events;
  MemoryStream stream;
  TestPacketStreamFilter filter(&events, &stream, false);
  stream.push(QByteArray::fromHex("00000001780000000279").toStdString());
  filter.filterEvent(Event(EventTypes::StreamInputReady, &stream));
  filter.filterEvent(Event(EventTypes::StreamInputShutdown, &stream));

  QVERIFY(events.posted.isEmpty());
  char value = '\0';
  QCOMPARE(filter.read(&value, 1), 1u);
  QCOMPARE(value, 'x');
  QVERIFY(!filter.isReady());
  QCOMPARE(events.posted, QList<EventTypes>{EventTypes::StreamInputFormatError});
}

void ClipboardChunksTests::packetFilterWritesCompleteSmallPacket_data()
{
  QTest::addColumn<QByteArray>("payload");
  QTest::addColumn<QByteArray>("expected");
  QTest::addColumn<int>("writes");

  QTest::newRow("empty write") << QByteArray{} << QByteArray{} << 0;
  QTest::newRow("mouse move") << QByteArray::fromHex("444d4d5600000123")
                              << QByteArray::fromHex("00000008444d4d5600000123") << 1;
  const QByteArray largePayload(512, 'x');
  QTest::newRow("large payload") << largePayload << QByteArray::fromHex("00000200") + largePayload << 2;
}

void ClipboardChunksTests::packetFilterWritesCompleteSmallPacket()
{
  QFETCH(QByteArray, payload);
  QFETCH(QByteArray, expected);
  QFETCH(int, writes);
  PacketEventQueue events;
  BufferWriteStream stream;
  PacketStreamFilter filter(&events, &stream, false);

  filter.write(payload.constData(), static_cast<uint32_t>(payload.size()));

  QCOMPARE(QByteArray::fromStdString(stream.str()), expected);
  QCOMPARE(stream.writes(), writes);
}

void ClipboardChunksTests::formatDataChunk()
{
  ClipboardID id = 0;
  uint32_t sequence = 1;
  uint32_t temp_m_chunk;
  std::string mockData("mock data");
  ClipboardChunk *chunk = ClipboardChunk::data(id, sequence, mockData);
  memcpy(&temp_m_chunk, &chunk->m_chunk[1], 4);

  QCOMPARE(chunk->m_chunk[0], id);
  QCOMPARE(temp_m_chunk, sequence);
  QCOMPARE(chunk->m_chunk[5], ChunkType::DataChunk);
  QCOMPARE(chunk->m_chunk[6], 'm');
  QCOMPARE(chunk->m_chunk[7], 'o');
  QCOMPARE(chunk->m_chunk[8], 'c');
  QCOMPARE(chunk->m_chunk[9], 'k');
  QCOMPARE(chunk->m_chunk[10], ' ');
  QCOMPARE(chunk->m_chunk[11], 'd');
  QCOMPARE(chunk->m_chunk[12], 'a');
  QCOMPARE(chunk->m_chunk[13], 't');
  QCOMPARE(chunk->m_chunk[14], 'a');
  QCOMPARE(chunk->m_chunk[15], '\0');

  delete chunk;
}

void ClipboardChunksTests::endFormatData()
{
  ClipboardID id = 1;
  uint32_t sequence = 1;
  uint32_t temp_m_chunk;
  ClipboardChunk *chunk = ClipboardChunk::end(id, sequence);
  memcpy(&temp_m_chunk, &chunk->m_chunk[1], 4);

  QCOMPARE(chunk->m_chunk[0], id);
  QCOMPARE(temp_m_chunk, sequence);
  QCOMPARE(chunk->m_chunk[5], ChunkType::DataEnd);
  QCOMPARE(chunk->m_chunk[6], '\0');

  delete chunk;
}

void ClipboardChunksTests::assembleAllowsDataAtExpectedSizeAndLimit()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "4"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "AB"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "CD"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataEnd, ""));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Finished);

  QCOMPARE(cached, std::string("ABCD"));
  QCOMPARE(id, static_cast<ClipboardID>(0));
  QCOMPARE(seq, static_cast<uint32_t>(7));
  QCOMPARE(ClipboardChunk::getExpectedSize(state), static_cast<size_t>(4));
  QVERIFY(!state.active);
}

void ClipboardChunksTests::assembleRejectsDataBeyondExpectedSize()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "1"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "AA"));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Error);
  QVERIFY(cached.empty());
  QCOMPARE(ClipboardChunk::getExpectedSize(state), static_cast<size_t>(0));
  QVERIFY(!state.active);
}

void ClipboardChunksTests::assembleRefusesExpectedSizeBeyondLimit()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "8"));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Oversize);
  QVERIFY(cached.empty());
}

void ClipboardChunksTests::assembleDiscardsOversizeTransferThenAcceptsNext()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "8"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "ABCD"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "EFGH"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataEnd, ""));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataStart, "4"));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataChunk, "WXYZ"));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataEnd, ""));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Oversize);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Oversize);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Oversize);
  QVERIFY(cached.empty());
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Oversize);
  QVERIFY(cached.empty());
  QVERIFY(!state.active);

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Finished);
  QCOMPARE(cached, std::string("WXYZ"));
  QCOMPARE(seq, static_cast<uint32_t>(8));
}

void ClipboardChunksTests::assembleAcceptsStartDuringOversizeTransfer()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "8"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "ABCD"));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataStart, "4"));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataChunk, "WXYZ"));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataEnd, ""));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Oversize);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Oversize);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Finished);
  QCOMPARE(cached, std::string("WXYZ"));
}

QTEST_MAIN(ClipboardChunksTests)
