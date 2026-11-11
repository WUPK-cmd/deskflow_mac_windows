/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/PacketStreamFilter.h"
#include "base/IEventQueue.h"
#include "deskflow/KeyTypes.h"
#include "deskflow/ProtocolTypes.h"

#include <cstring>

//
// PacketStreamFilter
//

PacketStreamFilter::PacketStreamFilter(IEventQueue *events, deskflow::IStream *stream, bool adoptStream)
    : StreamFilter(events, stream, adoptStream),
      m_events(events)
{
  // do nothing
}

PacketStreamFilter::~PacketStreamFilter()
{
  std::scoped_lock lock{m_mutex};
  cancelOutput();
}

void PacketStreamFilter::close()
{
  std::scoped_lock lock{m_mutex};
  m_size = 0;
  m_buffer.pop(m_buffer.getSize());
  cancelOutput();
  StreamFilter::close();
}

uint32_t PacketStreamFilter::read(void *buffer, uint32_t n)
{
  if (n == 0) {
    return 0;
  }

  std::scoped_lock lock{m_mutex};

  // if not enough data yet then give up
  if (!isReadyNoLock()) {
    return 0;
  }

  // read no more than what's left in the buffered packet
  if (n > m_size) {
    n = m_size;
  }

  // read it
  if (buffer != nullptr) {
    memcpy(buffer, m_buffer.peek(n), n);
  }
  m_buffer.pop(n);
  m_size -= n;

  // get next packet's size if we've finished with this packet and
  // there's enough data to do so.
  if (readPacketSize() && m_inputShutdown && !isReadyNoLock()) {
    if (m_size != 0 || m_buffer.getSize() != 0) {
      rejectInput();
    } else {
      m_events->addEvent(Event(EventTypes::StreamInputShutdown, getEventTarget()));
    }
  }

  return n;
}

void PacketStreamFilter::write(const void *buffer, uint32_t count)
{
  if (count == 0)
    return;

  const auto *data = static_cast<const uint8_t *>(buffer);
  std::scoped_lock lock{m_mutex};
  if (m_outputShutdown) {
    m_events->addEvent(Event(EventTypes::StreamOutputError, getEventTarget()));
    return;
  }

  if (count == 5) {
    if (memcmp(data, kMsgDMouseDown, 4) == 0)
      m_mouseButtons.set(data[4]);
    else if (memcmp(data, kMsgDMouseUp, 4) == 0)
      m_mouseButtons.reset(data[4]);
  }

  const bool clipboard = count >= 4 && memcmp(data, kMsgDClipboard, 4) == 0;
  // Keep the uncontended input path allocation-free, including input arriving
  // during a clipboard cooldown when there are no earlier queued dependencies.
  if (m_output.empty() && !getStream()->hasPendingOutput() && !(clipboard && m_bulkTimer)) {
    writePacket(buffer, count);
    return;
  }

  OutputPacket packet{
      std::vector<uint8_t>(data, data + count),
      count == 8 && memcmp(data, kMsgDMouseMove, 4) == 0 && m_mouseButtons.none(), clipboard
  };
  if (count == 4 && memcmp(data, kMsgCKeepAlive, 4) == 0) {
    // Heartbeats carry no input/clipboard dependency and must survive even a
    // paste barrier queued behind a slow bulk transfer.
    m_output.push_front(std::move(packet));
  } else if (isPriorityInput(data, count)) {
    // Cross only trailing unsent clipboard packets. Earlier input and modified
    // keyboard chords remain barriers, preserving their relative order.
    auto start = m_output.size();
    while (start > 0 && m_output[start - 1].clipboard)
      --start;

    if (packet.position && start > 0 && m_output[start - 1].position)
      m_output[start - 1] = std::move(packet);
    else
      m_output.insert(m_output.begin() + static_cast<std::ptrdiff_t>(start), std::move(packet));
  } else {
    m_output.push_back(std::move(packet));
  }
  drainOutput();
}

bool PacketStreamFilter::isPriorityInput(const uint8_t *data, uint32_t count)
{
  if (count < 4)
    return false;
  if (memcmp(data, kMsgDMouseMove, 4) == 0 || memcmp(data, kMsgDMouseRelMove, 4) == 0 ||
      memcmp(data, kMsgDMouseDown, 4) == 0 || memcmp(data, kMsgDMouseUp, 4) == 0 ||
      memcmp(data, kMsgDMouseWheel, 4) == 0)
    return true;

  // All supported key formats retain key and modifier fields at these offsets,
  // including legacy key messages and language-bearing DKDL/DKRP messages.
  const bool keyMessage = memcmp(data, kMsgDKeyDown1_0, 4) == 0 || memcmp(data, kMsgDKeyDown, 4) == 0 ||
                          memcmp(data, kMsgDKeyRepeat, 4) == 0 || memcmp(data, kMsgDKeyUp, 4) == 0;
  if (!keyMessage || count < 8)
    return false;
  const auto key = (uint32_t(data[4]) << 8) | uint32_t(data[5]);
  const auto mask = (uint32_t(data[6]) << 8) | uint32_t(data[7]);
  constexpr auto dependencyModifiers =
      KeyModifierControl | KeyModifierAlt | KeyModifierMeta | KeyModifierSuper | KeyModifierAltGr;
  if (mask & dependencyModifiers)
    return false;
  if ((key >= kKeyControl_L && key <= kKeyControl_R) || (key >= kKeyMeta_L && key <= kKeySuper_R) || key == kKeyAltGr)
    return false;
  if ((mask & KeyModifierShift) &&
      (key == kKeyInsert || key == kKeyDelete || key == kKeyKP_Insert || key == kKeyKP_Delete))
    return false;
  return true;
}

void PacketStreamFilter::writePacket(const void *buffer, uint32_t count, bool paceBulk)
{
  constexpr uint32_t headerSize = 4;
  constexpr uint32_t smallPayloadSize = 64;
  uint8_t packet[headerSize + smallPayloadSize];
  packet[0] = static_cast<uint8_t>(count >> 24);
  packet[1] = static_cast<uint8_t>(count >> 16);
  packet[2] = static_cast<uint8_t>(count >> 8);
  packet[3] = static_cast<uint8_t>(count);

  // Enqueue small input packets in one write so the socket cannot send the header alone.
  if (count <= smallPayloadSize) {
    memcpy(packet + headerSize, buffer, count);
    getStream()->write(packet, headerSize + count);
  } else {
    getStream()->write(packet, headerSize);
    getStream()->write(buffer, count);
  }

  const auto *data = static_cast<const uint8_t *>(buffer);
  if (paceBulk && count >= 10 && memcmp(data, kMsgDClipboard, 4) == 0 && data[9] == ChunkType::DataChunk)
    startBulkTimer();
}

void PacketStreamFilter::drainOutput()
{
  // A buffered socket resumes here on output completion. Unbuffered streams can
  // consume the queue immediately without relying on a completion notification.
  while (!m_outputShutdown && !m_output.empty() && !getStream()->hasPendingOutput()) {
    if (m_output.front().clipboard && m_bulkTimer)
      break;
    auto packet = std::move(m_output.front());
    m_output.pop_front();
    writePacket(packet.data.data(), static_cast<uint32_t>(packet.data.size()));
  }
}

void PacketStreamFilter::startBulkTimer()
{
  if (m_bulkTimer)
    return;
  // One existing 16 KiB chunk per five-millisecond turn bounds bulk feeding.
  // Socket/TLS completion alone cannot pour the whole transfer into the kernel.
  // Input already ahead of the next chunk remains runnable during this pause.
  m_bulkTimer = m_events->newOneShotTimer(0.005, nullptr);
  if (!m_bulkTimer)
    return;
  m_events->addHandler(EventTypes::Timer, m_bulkTimer, [this](const auto &) {
    std::scoped_lock lock{m_mutex};
    cancelBulkTimer();
    const bool wasQueued = !m_output.empty();
    drainOutput();
    if (wasQueued && m_output.empty() && !getStream()->hasPendingOutput())
      m_events->addEvent(Event(EventTypes::StreamOutputFlushed, getEventTarget()));
  });
}

void PacketStreamFilter::cancelBulkTimer()
{
  if (!m_bulkTimer)
    return;
  m_events->removeHandler(EventTypes::Timer, m_bulkTimer);
  m_events->deleteTimer(m_bulkTimer);
  m_bulkTimer = nullptr;
}

void PacketStreamFilter::cancelOutput()
{
  m_outputShutdown = true;
  cancelBulkTimer();
  m_output.clear();
  m_mouseButtons.reset();
}

void PacketStreamFilter::flush()
{
  {
    std::scoped_lock lock{m_mutex};
    cancelBulkTimer();
    while (!m_output.empty()) {
      auto packet = std::move(m_output.front());
      m_output.pop_front();
      writePacket(packet.data.data(), static_cast<uint32_t>(packet.data.size()), false);
    }
  }
  StreamFilter::flush();
}

void PacketStreamFilter::shutdownOutput()
{
  std::scoped_lock lock{m_mutex};
  cancelOutput();
  StreamFilter::shutdownOutput();
}

bool PacketStreamFilter::hasPendingOutput() const
{
  std::scoped_lock lock{m_mutex};
  return !m_output.empty() || getStream()->hasPendingOutput();
}

void PacketStreamFilter::shutdownInput()
{
  std::scoped_lock lock{m_mutex};
  m_size = 0;
  m_buffer.pop(m_buffer.getSize());
  StreamFilter::shutdownInput();
}

bool PacketStreamFilter::isReady() const
{
  std::scoped_lock lock{m_mutex};
  return isReadyNoLock();
}

uint32_t PacketStreamFilter::getSize() const
{
  std::scoped_lock lock{m_mutex};
  return isReadyNoLock() ? m_size : 0;
}

bool PacketStreamFilter::isReadyNoLock() const
{
  return (m_size != 0 && m_buffer.getSize() >= m_size);
}

bool PacketStreamFilter::readPacketSize()
{
  // note -- m_mutex must be locked on entry

  if (m_size == 0 && m_buffer.getSize() >= 4) {
    uint8_t buffer[4];
    memcpy(buffer, m_buffer.peek(sizeof(buffer)), sizeof(buffer));
    m_buffer.pop(sizeof(buffer));
    m_size =
        ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) | ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
    if (m_size == 0 || m_size > PROTOCOL_MAX_MESSAGE_LENGTH) {
      rejectInput();
      return false;
    }
  }
  return true;
}

void PacketStreamFilter::rejectInput()
{
  m_inputFailed = true;
  m_size = 0;
  m_buffer.pop(m_buffer.getSize());
  m_events->addEvent(Event(EventTypes::StreamInputFormatError, getEventTarget()));
}

bool PacketStreamFilter::readMore()
{
  if (m_inputFailed)
    return false;

  // note if we have whole packet
  bool wasReady = isReadyNoLock();

  // read more data
  char buffer[4096];
  uint32_t n = getStream()->read(buffer, sizeof(buffer));
  while (n > 0) {
    m_buffer.write(buffer, n);

    // if we don't yet have the next packet size then get it, if possible.
    // Note that we can't wait for whole pending data to arrive because it may be huge in
    // case of malicious or erroneous peer.
    if (!readPacketSize()) {
      return false;
    }

    n = getStream()->read(buffer, sizeof(buffer));
  }

  // note if we now have a whole packet
  bool isReady = isReadyNoLock();

  // if we weren't ready before but now we are then send a
  // input ready event apparently from the filtered stream.
  return (wasReady != isReady);
}

void PacketStreamFilter::filterEvent(const Event &event)
{
  if (event.getType() == EventTypes::StreamOutputFlushed) {
    std::scoped_lock lock{m_mutex};
    drainOutput();
    if (!m_output.empty() || getStream()->hasPendingOutput())
      return;
  } else if (event.getType() == EventTypes::StreamOutputError || event.getType() == EventTypes::StreamOutputShutdown ||
             event.getType() == EventTypes::SocketDisconnected) {
    std::scoped_lock lock{m_mutex};
    cancelOutput();
  } else if (event.getType() == EventTypes::StreamInputReady) {
    std::scoped_lock lock{m_mutex};
    if (!readMore()) {
      return;
    }
  } else if (event.getType() == EventTypes::StreamInputShutdown) {
    // discard this if we have buffered data
    std::scoped_lock lock{m_mutex};
    if (m_inputFailed)
      return;
    m_inputShutdown = true;
    if (isReadyNoLock()) {
      // Let the consumer read buffered packets before delivering shutdown.
      return;
    }
    if (m_size != 0 || m_buffer.getSize() != 0) {
      rejectInput();
      return;
    }
  }

  // pass event
  StreamFilter::filterEvent(event);
}
