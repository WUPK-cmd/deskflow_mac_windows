/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "io/StreamBuffer.h"
#include "io/StreamFilter.h"

#include <bitset>
#include <deque>
#include <mutex>
#include <vector>

class IEventQueue;

//! Packetizing stream filter
/*!
Filters a stream to read and write packets.
*/
class PacketStreamFilter : public StreamFilter
{
public:
  PacketStreamFilter(IEventQueue *events, deskflow::IStream *stream, bool adoptStream = true);
  ~PacketStreamFilter() override = default;

  // IStream overrides
  void close() override;
  uint32_t read(void *buffer, uint32_t n) override;
  void write(const void *buffer, uint32_t n) override;
  void flush() override;
  void shutdownInput() override;
  void shutdownOutput() override;
  bool isReady() const override;
  uint32_t getSize() const override;
  bool hasPendingOutput() const override;

protected:
  // StreamFilter overrides
  void filterEvent(const Event &) override;

private:
  bool isReadyNoLock() const;
  bool readPacketSize();
  bool readMore();
  void rejectInput();
  void writePacket(const void *buffer, uint32_t count);
  void drainOutput();
  void cancelOutput();

  struct OutputPacket
  {
    std::vector<uint8_t> data;
    bool position;
    bool clipboard;
  };

private:
  mutable std::mutex m_mutex;
  uint32_t m_size = 0;
  StreamBuffer m_buffer;
  bool m_inputShutdown = false;
  bool m_inputFailed = false;
  std::deque<OutputPacket> m_output;
  std::bitset<256> m_mouseButtons;
  bool m_outputShutdown = false;
  IEventQueue *m_events = nullptr;
};
