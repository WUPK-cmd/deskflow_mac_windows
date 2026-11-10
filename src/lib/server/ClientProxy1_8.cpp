/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2021 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"
#include "deskflow/KeyboardLayoutManager.h"
#include "deskflow/Microphone.h"
#include "deskflow/ProtocolUtil.h"
#include "io/StreamFilter.h"
#include "net/TCPSocket.h"

#include <cstring>

#include "ClientProxy1_8.h"

ClientProxy1_8::ClientProxy1_8(
    const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events,
    QString verifiedFingerprint
)
    : ClientProxy1_7(name, adoptedStream, server, events),
      m_verifiedFingerprint(std::move(verifiedFingerprint))
{
  synchronizeLanguages();
}

void ClientProxy1_8::offerMicrophone(OptionsList &options, const QString &targetFingerprint)
{
  m_microphoneOffered = !targetFingerprint.isEmpty() && targetFingerprint == m_verifiedFingerprint;
  m_microphoneReady = false;
  if (m_microphoneOffered) {
    options.push_back(kOptionMicrophoneFollow);
    options.push_back(1);
  }
}

bool ClientProxy1_8::acceptsMicrophone() const
{
  return m_microphoneOffered && m_microphoneReady;
}

void ClientProxy1_8::sendMicrophone(const std::string &pcm)
{
  if (!acceptsMicrophone() || m_microphoneFramesPending >= 2 || pcm.size() != deskflow::Microphone::kFrameBytes)
    return;

  const auto *filter = dynamic_cast<StreamFilter *>(getStream());
  const auto *socket = filter ? dynamic_cast<TCPSocket *>(filter->getStream()) : nullptr;
  if (!socket || socket->hasPendingOutput())
    return;

  // Bound unacknowledged speech so it cannot accumulate ahead of input during a network stall.
  ++m_microphoneFramesPending;
  ProtocolUtil::writef(getStream(), deskflow::Microphone::kFrameMessage, &pcm);
}

bool ClientProxy1_8::parseMessage(const uint8_t *code)
{
  if (std::memcmp(code, deskflow::Microphone::kReadyMessage, 4) == 0) {
    m_microphoneReady = m_microphoneOffered;
    return true;
  }
  if (std::memcmp(code, deskflow::Microphone::kAckMessage, 4) == 0) {
    if (m_microphoneFramesPending)
      --m_microphoneFramesPending;
    return true;
  }
  return ClientProxy1_7::parseMessage(code);
}

void ClientProxy1_8::synchronizeLanguages() const
{
  deskflow::KeyboardLayoutManager layoutManager;
  auto localLayouts = layoutManager.getSerializedLocalLayouts();
  if (!localLayouts.empty()) {
    LOG_VERBOSE("send server languages to the client: %s", localLayouts.c_str());
    ProtocolUtil::writef(getStream(), kMsgDLanguageSynchronisation, &localLayouts);
  } else {
    LOG_ERR("failed to read server languages");
  }
}

void ClientProxy1_8::keyDown(KeyID key, KeyModifierMask mask, KeyButton button, const std::string &language)
{
  LOG_VERBOSE(
      "send key down to \"%s\" id=%d, mask=0x%04x, button=0x%04x, layout=%s", getName().c_str(), key, mask, button,
      language.c_str()
  );
  ProtocolUtil::writef(getStream(), kMsgDKeyDown, key, mask, button, &language);
}

void ClientProxy1_8::keyRepeat(
    KeyID key, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &language
)
{
  LOG_VERBOSE(
      "send key repeat to \"%s\" id=%d, mask=0x%04x, count=%d, button=0x%04x, layout=%s", getName().c_str(), key, mask,
      count, button, language.c_str()
  );
  ProtocolUtil::writef(getStream(), kMsgDKeyRepeat, key, mask, count, button, &language);
}
