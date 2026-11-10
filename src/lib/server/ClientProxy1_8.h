/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2021 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "server/ClientProxy1_7.h"

#include <QString>

class ClientProxy1_8 : public ClientProxy1_7
{
public:
  ClientProxy1_8(
      const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events,
      QString verifiedFingerprint
  );
  ~ClientProxy1_8() override = default;

  void keyDown(KeyID, KeyModifierMask, KeyButton, const std::string &) override;
  void keyRepeat(KeyID, KeyModifierMask, int32_t count, KeyButton, const std::string &) override;
  void offerMicrophone(OptionsList &options, const QString &targetFingerprint);
  bool acceptsMicrophone() const;
  void sendMicrophone(const std::string &pcm);

protected:
  bool parseMessage(const uint8_t *code) override;

private:
  void synchronizeLanguages() const;
  const QString m_verifiedFingerprint;
  bool m_microphoneOffered = false;
  bool m_microphoneReady = false;
  unsigned m_microphoneFramesPending = 0;
};
