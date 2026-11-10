/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/StreamChunker.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"

#include <algorithm>

static constexpr size_t kChunkSize = 512 * 1024;

void StreamChunker::sendClipboard(
    std::string_view data, ClipboardID id, uint32_t sequence, IEventQueue *events, void *eventTarget
)
{
  const auto size = data.size();
  auto *start = ClipboardChunk::start(id, sequence, QString::number(size).toStdString());
  events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, start));

  do {
    const auto chunkSize = std::min(data.size(), kChunkSize);
    auto *chunk = ClipboardChunk::data(id, sequence, data.substr(0, chunkSize));
    events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, chunk));
    data.remove_prefix(chunkSize);
  } while (!data.empty());

  auto *end = ClipboardChunk::end(id, sequence);
  events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, end));
  LOG_DEBUG("queued clipboard size=%zu", size);
}
