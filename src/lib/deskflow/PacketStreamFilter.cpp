/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/PacketStreamFilter.h"
#include "base/IEventQueue.h"
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

void PacketStreamFilter::close()
{
  std::scoped_lock lock{m_mutex};
  m_size = 0;
  m_buffer.pop(m_buffer.getSize());
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
    return;
  }

  getStream()->write(packet, headerSize);
  getStream()->write(buffer, count);
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
  if (event.getType() == EventTypes::StreamInputReady) {
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
