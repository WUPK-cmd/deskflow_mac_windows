// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "deskflow/Microphone.h"
#include "base/Log.h"

#ifdef DESKFLOW_MICROPHONE

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef Q_OS_WIN
#include <objbase.h>
#endif
#endif

namespace deskflow {

#ifdef DESKFLOW_MICROPHONE
namespace {

#ifdef Q_OS_WIN
class AudioComApartment
{
public:
  ~AudioComApartment()
  {
    if (SUCCEEDED(m_result))
      CoUninitialize();
  }

  bool ready() const
  {
    return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
  }

private:
  const HRESULT m_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
};
#endif

QAudioDevice findDevice(const QList<QAudioDevice> &devices, const QString &name)
{
  for (const auto &device : devices) {
    if (device.description() == name)
      return device;
  }
  LOG_WARN("microphone audio device not found: %s", qPrintable(name));
  for (const auto &device : devices)
    LOG_INFO("available audio device: %s", qPrintable(device.description()));
  return {};
}

QAudioFormat findFormat(const QAudioDevice &device)
{
  const auto preferred = device.preferredFormat();
  if (preferred.isValid() && preferred.sampleRate() == 48000 && preferred.channelCount() <= 2 &&
      preferred.sampleFormat() != QAudioFormat::UInt8)
    return preferred;

  for (const auto channels : {1, 2}) {
    for (const auto sampleFormat : {QAudioFormat::Int16, QAudioFormat::Float, QAudioFormat::Int32}) {
      QAudioFormat format;
      format.setSampleRate(48000);
      format.setChannelCount(channels);
      format.setSampleFormat(sampleFormat);
      if (device.isFormatSupported(format))
        return format;
    }
  }
  LOG_WARN("microphone audio device must support 48 kHz mono or stereo: %s", qPrintable(device.description()));
  return {};
}

QByteArray encodeFrame(const QByteArray &data, const QAudioFormat &format)
{
  QByteArray frame(Microphone::kFrameBytes, Qt::Uninitialized);
  for (int i = 0; i < Microphone::kSamples; ++i) {
    const char *sample = data.constData() + i * format.bytesPerFrame();
    float value = 0;
    for (int channel = 0; channel < format.channelCount(); ++channel)
      value += format.normalizedSampleValue(sample + channel * format.bytesPerSample());
    value /= format.channelCount();
    const auto pcm = std::isfinite(value) ? qint16(std::clamp(value, -1.0f, 1.0f) * 32767) : qint16(0);
    qToLittleEndian(pcm, frame.data() + i * sizeof(qint16));
  }
  return frame;
}

QByteArray decodeFrame(const QByteArray &frame, const QAudioFormat &format)
{
  QByteArray data(Microphone::kSamples * format.bytesPerFrame(), Qt::Uninitialized);
  char *sample = data.data();
  for (int i = 0; i < Microphone::kSamples; ++i) {
    const auto pcm = qFromLittleEndian<qint16>(frame.constData() + i * sizeof(qint16));
    for (int channel = 0; channel < format.channelCount(); ++channel) {
      if (format.sampleFormat() == QAudioFormat::Int16) {
        std::memcpy(sample, &pcm, sizeof(pcm));
      } else if (format.sampleFormat() == QAudioFormat::Float) {
        const float value = pcm / 32768.0f;
        std::memcpy(sample, &value, sizeof(value));
      } else {
        const qint32 value = pcm * 65536;
        std::memcpy(sample, &value, sizeof(value));
      }
      sample += format.bytesPerSample();
    }
  }
  return data;
}

} // namespace

class Microphone::Worker final : public QThread
{
public:
  Worker(QAudioDevice input, QAudioFormat inputFormat, QAudioDevice output, QAudioFormat outputFormat)
      : m_input(std::move(input)),
        m_inputFormat(inputFormat),
        m_output(std::move(output)),
        m_outputFormat(outputFormat)
  {
  }

  std::mutex mutex;
  Route route = Route::Muted;
  unsigned routeRevision = 0;
  QByteArray frame;

protected:
  void run() override
  {
#ifdef Q_OS_WIN
    const AudioComApartment com;
    if (!com.ready()) {
      LOG_WARN("could not initialize Windows audio thread");
      return;
    }
#endif
    std::unique_ptr<QAudioSource> source;
    if (!m_input.isNull()) {
      source = std::make_unique<QAudioSource>(m_input, m_inputFormat);
      source->setBufferSize(3 * kSamples * m_inputFormat.bytesPerFrame());
    }
    QAudioSink sink(m_output, m_outputFormat);
    const auto outputBytes = kSamples * m_outputFormat.bytesPerFrame();
    sink.setBufferSize(2 * outputBytes);

    QIODevice *input = nullptr;
    QIODevice *output = nullptr;
    unsigned revision = 0;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &timer, [&] {
      QByteArray pcm;
      Route current;
      bool switched;
      {
        const std::lock_guard lock(mutex);
        current = route;
        switched = revision != routeRevision;
        revision = routeRevision;
        if (!source && current == Route::Local)
          pcm.swap(frame);
      }
      if (switched) {
        sink.reset();
        output = current == Route::Local ? sink.start() : nullptr;
        if (!output && current == Route::Local)
          LOG_WARN("could not start microphone playback");
        if (source) {
          source->reset();
          input = current == Route::Muted ? nullptr : source->start();
          if (!input && current != Route::Muted)
            LOG_WARN("could not start microphone capture");
        }
      }
      if (input) {
        const auto inputBytes = kSamples * m_inputFormat.bytesPerFrame();
        const auto availableFrames = input->bytesAvailable() / inputBytes;
        if (!availableFrames)
          return;
        if (availableFrames > 1)
          input->skip((availableFrames - 1) * inputBytes);
        const auto data = input->read(inputBytes);
        if (data.size() != inputBytes)
          return;
        pcm = encodeFrame(data, m_inputFormat);
      }
      if (pcm.isEmpty())
        return;

      const std::lock_guard lock(mutex);
      if (revision != routeRevision)
        return;
      if (current == Route::Remote) {
        frame = std::move(pcm);
      } else if (current == Route::Local && output && sink.bytesFree() >= outputBytes) {
        const auto data = decodeFrame(pcm, m_outputFormat);
        output->write(data);
      }
    });
    timer.start(10);
    exec();
  }

private:
  const QAudioDevice m_input;
  const QAudioFormat m_inputFormat;
  const QAudioDevice m_output;
  const QAudioFormat m_outputFormat;
};

std::unique_ptr<Microphone> Microphone::open(const QString &inputName, const QString &outputName)
{
  if (outputName.isEmpty())
    return {};
#ifdef Q_OS_WIN
  const AudioComApartment com;
  if (!com.ready()) {
    LOG_WARN("could not initialize Windows audio device discovery");
    return {};
  }
#endif
  const auto output = findDevice(QMediaDevices::audioOutputs(), outputName);
  if (output.isNull())
    return {};
  const auto outputFormat = findFormat(output);
  if (!outputFormat.isValid())
    return {};

  QAudioDevice input;
  QAudioFormat inputFormat;
  if (!inputName.isEmpty()) {
    input = findDevice(QMediaDevices::audioInputs(), inputName);
    if (input.isNull())
      return {};
    inputFormat = findFormat(input);
    if (!inputFormat.isValid())
      return {};
  }

  return std::unique_ptr<Microphone>(new Microphone(std::make_unique<Worker>(input, inputFormat, output, outputFormat))
  );
}

Microphone::Microphone(std::unique_ptr<Worker> worker) : m_worker(std::move(worker))
{
  m_worker->start();
}

Microphone::~Microphone()
{
  m_worker->quit();
  m_worker->wait();
}

void Microphone::setRoute(Route route)
{
  const std::lock_guard lock(m_worker->mutex);
  if (route != m_worker->route) {
    m_worker->route = route;
    ++m_worker->routeRevision;
    m_worker->frame.clear();
  }
}

QByteArray Microphone::takeFrame()
{
  const std::lock_guard lock(m_worker->mutex);
  QByteArray frame;
  if (m_worker->route == Route::Remote)
    frame.swap(m_worker->frame);
  return frame;
}

void Microphone::receiveFrame(QByteArray frame)
{
  const std::lock_guard lock(m_worker->mutex);
  if (m_worker->route == Route::Local && frame.size() == kFrameBytes)
    m_worker->frame = std::move(frame);
}
#else
class Microphone::Worker
{
};

std::unique_ptr<Microphone> Microphone::open(const QString &, const QString &)
{
  LOG_WARN("this Deskflow build does not include microphone switching");
  return {};
}

Microphone::~Microphone() = default;
void Microphone::setRoute(Route)
{
}

QByteArray Microphone::takeFrame()
{
  return {};
}

void Microphone::receiveFrame(QByteArray)
{
}
#endif

} // namespace deskflow
