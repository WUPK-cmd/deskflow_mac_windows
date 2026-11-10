// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#pragma once

#include <QByteArray>

#include <memory>

class QString;

namespace deskflow {

class Microphone
{
public:
  enum class Route
  {
    Muted,
    Local,
    Remote
  };

  // The wire format is 20 ms of 48 kHz mono signed 16-bit little-endian PCM.
  static constexpr int kSamples = 960;
  static constexpr int kFrameBytes = kSamples * 2;
  static constexpr auto kFrameMessage = "DMIC%s";
  static constexpr auto kReadyMessage = "CMIR";
  static constexpr auto kAckMessage = "CMIA";

  static std::unique_ptr<Microphone> open(const QString &inputName, const QString &outputName);
  ~Microphone();

  void setRoute(Route route);
  QByteArray takeFrame();
  void receiveFrame(QByteArray frame);

private:
  class Worker;
  explicit Microphone(std::unique_ptr<Worker> worker);
  std::unique_ptr<Worker> m_worker;
};

} // namespace deskflow
