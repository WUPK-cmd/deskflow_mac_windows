// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "OutputLoopbackTests.h"

#include "MockEventQueue.h"
#include "common/Settings.h"
#include "deskflow/KeyTypes.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "net/FingerprintDatabase.h"
#include "net/NetworkAddress.h"
#include "net/SecureSocket.h"
#include "net/SecureUtils.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace {

// SocketMultiplexer posts from its own thread. Run handlers only on the test thread,
// exactly as the application's event loop does, and release each event's payload once.
class LoopbackEventQueue : public MockEventQueue
{
public:
  struct Timer
  {
    std::chrono::steady_clock::time_point deadline;
    void *target;
    bool posted = false;
  };
  EventQueueTimer *newOneShotTimer(double duration, void *target) override
  {
    auto timer = std::make_unique<Timer>();
    timer->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(qRound64(duration * 1000));
    auto *key = reinterpret_cast<EventQueueTimer *>(timer.get());
    timer->target = target ? target : key;
    m_timers.emplace(key, std::move(timer));
    return key;
  }
  void deleteTimer(EventQueueTimer *timer) override
  {
    m_timers.erase(timer);
  }

  ~LoopbackEventQueue() override
  {
    for (const auto &event : m_pending)
      Event::deleteData(event);
  }

  void addEvent(Event &&event) override
  {
    const std::lock_guard lock(m_mutex);
    m_pending.push_back(std::move(event));
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[{type, target}] = handler;
  }

  void removeHandler(EventTypes type, void *target) override
  {
    m_handlers.erase({type, target});
  }

  void removeHandlers(void *target) override
  {
    for (auto it = m_handlers.begin(); it != m_handlers.end();) {
      if (it->first.second == target)
        it = m_handlers.erase(it);
      else
        ++it;
    }
  }

  bool dispatchEvent(const Event &event) override
  {
    auto handler = m_handlers.find({event.getType(), event.getTarget()});
    if (handler == m_handlers.end())
      handler = m_handlers.find({EventTypes::Unknown, event.getTarget()});
    if (handler == m_handlers.end())
      return false;
    const auto callback = handler->second;
    callback(event);
    return true;
  }

  void drain()
  {
    const auto now = std::chrono::steady_clock::now();
    for (const auto &[key, timer] : m_timers) {
      if (!timer->posted && now >= timer->deadline) {
        timer->posted = true;
        addEvent(Event(EventTypes::Timer, timer->target));
      }
    }
    // A bounded turn lets the Qt peer make progress even under a stream of flush events.
    for (int i = 0; i < 256; ++i) {
      Event event;
      {
        const std::lock_guard lock(m_mutex);
        if (m_pending.empty())
          return;
        event = std::move(m_pending.front());
        m_pending.pop_front();
      }
      dispatchEvent(event);
      Event::deleteData(event);
    }
  }

private:
  struct HandlerLess
  {
    bool operator()(const std::pair<EventTypes, void *> &a, const std::pair<EventTypes, void *> &b) const
    {
      if (a.first != b.first)
        return a.first < b.first;
      return std::less<void *>{}(a.second, b.second);
    }
  };

  std::mutex m_mutex;
  std::deque<Event> m_pending;
  std::map<std::pair<EventTypes, void *>, EventHandler, HandlerLess> m_handlers;
  std::map<EventQueueTimer *, std::unique_ptr<Timer>> m_timers;
};

class ProbeServer : public QTcpServer
{
public:
  bool tls = false;
  QSslCertificate certificate;
  QSslKey key;
  QSslSocket *tlsPeer = nullptr;

protected:
  void incomingConnection(qintptr descriptor) override
  {
    if (!tls) {
      QTcpServer::incomingConnection(descriptor);
      return;
    }
    tlsPeer = new QSslSocket(this);
    tlsPeer->setLocalCertificate(certificate);
    tlsPeer->setPrivateKey(key);
    tlsPeer->setPeerVerifyMode(QSslSocket::VerifyNone);
    tlsPeer->setSocketDescriptor(descriptor);
    tlsPeer->startServerEncryption();
  }
};

bool waitFor(LoopbackEventQueue &events, const std::function<bool()> &condition)
{
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 10000) {
    events.drain();
    QCoreApplication::processEvents();
    if (condition())
      return true;
    QTest::qWait(1);
  }
  return false;
}

} // namespace

void OutputLoopbackTests::initTestCase()
{
#ifdef Q_OS_WIN
  m_arch.init();
#endif
  m_log.setFilter(LogLevel::Level::Warning);
}

void OutputLoopbackTests::realTcpStreamPreservesClipboardAndInputOrdering()
{
  // Only a new ephemeral localhost endpoint is used. No desktop input or OS clipboard
  // API is called, and the user's Deskflow listener is never contacted.
  QTcpServer listener;
  listener.setProxy(QNetworkProxy::NoProxy);
  QVERIFY2(listener.listen(QHostAddress::LocalHost, 0), qPrintable(listener.errorString()));

  LoopbackEventQueue events;
  SocketMultiplexer multiplexer;
  auto *socket = new TCPSocket(&events, &multiplexer);
  PacketStreamFilter stream(&events, socket, true);
  bool connected = false;
  bool failed = false;
  events.addHandler(EventTypes::DataSocketConnected, stream.getEventTarget(), [&](const Event &) { connected = true; });
  events.addHandler(EventTypes::DataSocketConnectionFailed, stream.getEventTarget(), [&](const Event &) {
    failed = true;
  });
  events.addHandler(EventTypes::StreamOutputError, stream.getEventTarget(), [&](const Event &) { failed = true; });
  NetworkAddress address("127.0.0.1", listener.serverPort());
  address.resolve();
  socket->connect(address);
  QVERIFY(waitFor(events, [&] { return failed || (connected && listener.hasPendingConnections()); }));
  QVERIFY(!failed);
  QVERIFY(connected);
  std::unique_ptr<QTcpSocket> peer(listener.nextPendingConnection());
  QVERIFY(peer != nullptr);
  peer->setReadBufferSize(4096);

  ProtocolUtil::writef(&stream, kMsgDMouseMove, 10, 20);
  std::string payload(256 * 1024, '\x5a');
  payload[0] = '\0';
  payload[16 * 1024 - 1] = '\xff';
  payload[16 * 1024] = '\0';
  auto declaredSize = std::to_string(payload.size());
  ProtocolUtil::writef(&stream, kMsgDClipboard, 0, 7, ChunkType::DataStart, &declaredSize);
  for (size_t offset = 0; offset < payload.size(); offset += 16 * 1024) {
    auto chunk = payload.substr(offset, 16 * 1024);
    ProtocolUtil::writef(&stream, kMsgDClipboard, 0, 7, ChunkType::DataChunk, &chunk);
  }
  const std::string empty;
  ProtocolUtil::writef(&stream, kMsgDClipboard, 0, 7, ChunkType::DataEnd, &empty);
  for (int i = 0; i < 100; ++i)
    ProtocolUtil::writef(&stream, kMsgDMouseMove, 100 + i, 200 + i);
  ProtocolUtil::writef(&stream, kMsgDKeyDown1_1, 0x76, KeyModifierControl, 0x2f);
  ProtocolUtil::writef(&stream, kMsgDMouseDown, 1);
  ProtocolUtil::writef(&stream, kMsgDMouseMove, 300, 220);
  ProtocolUtil::writef(&stream, kMsgDMouseMove, 310, 230);
  ProtocolUtil::writef(&stream, kMsgDMouseUp, 1);
  ProtocolUtil::writef(&stream, kMsgDMouseMove, 320, 240);
  ProtocolUtil::writef(&stream, kMsgCLeave);

  // Deliberately let the peer's read buffer remain full for a short interval. Exact
  // coalescing counts are covered by deterministic tests, not OS buffer timing.
  QTest::qWait(30);
  QByteArray buffered;
  std::vector<QByteArray> packets;
  const auto receive = [&] {
    buffered.append(peer->readAll());
    while (buffered.size() >= 4) {
      const auto size = qFromBigEndian<quint32>(buffered.constData());
      if (size > 1024 * 1024)
        return true;
      if (buffered.size() < 4 + qsizetype(size))
        break;
      packets.push_back(buffered.mid(4, size));
      buffered.remove(0, 4 + size);
    }
    return failed || (!packets.empty() && packets.back() == QByteArray("COUT"));
  };
  QVERIFY(waitFor(events, receive));
  QVERIFY(!failed);
  QVERIFY(buffered.isEmpty());
  QVERIFY(!packets.empty());
  QCOMPARE(packets.back(), QByteArray("COUT"));

  QByteArray restoredClipboard;
  int clipboardEnd = -1;
  int paste = -1;
  int lastPointerBeforePaste = -1;
  int down = -1;
  int up = -1;
  int dragMoves = 0;
  bool started = false;
  for (size_t i = 0; i < packets.size(); ++i) {
    const auto &packet = packets[i];
    const auto tag = packet.first(4);
    if (tag == "DCLP") {
      QVERIFY(packet.size() >= 14);
      QCOMPARE(quint8(packet[4]), quint8(0));
      QCOMPARE(qFromBigEndian<quint32>(packet.constData() + 5), quint32(7));
      const auto size = qFromBigEndian<quint32>(packet.constData() + 10);
      QCOMPARE(packet.size(), qsizetype(14 + size));
      const auto data = packet.mid(14);
      switch (quint8(packet[9])) {
      case 1:
        QVERIFY(!started);
        started = true;
        QCOMPARE(data, QByteArray("262144"));
        break;
      case 2:
        QVERIFY(started);
        QVERIFY(clipboardEnd < 0);
        restoredClipboard += data;
        break;
      case 3:
        QVERIFY(started);
        QVERIFY(clipboardEnd < 0);
        clipboardEnd = int(i);
        QVERIFY(data.isEmpty());
        break;
      default:
        QFAIL("Unexpected clipboard chunk mark");
      }
    } else if (tag == "DKDN") {
      paste = int(i);
      QCOMPARE(packet, QByteArray::fromHex("444b444e00760002002f"));
      QVERIFY(clipboardEnd >= 0 && clipboardEnd < paste);
      QVERIFY(lastPointerBeforePaste >= 0);
      QCOMPARE(packets[size_t(lastPointerBeforePaste)], QByteArray::fromHex("444d4d5600c7012b"));
    } else if (tag == "DMDN") {
      down = int(i);
    } else if (tag == "DMUP") {
      up = int(i);
    } else if (tag == "DMMV") {
      if (paste < 0)
        lastPointerBeforePaste = int(i);
      if (down >= 0 && up < 0)
        ++dragMoves;
    }
  }
  QCOMPARE(restoredClipboard, QByteArray::fromStdString(payload));
  QVERIFY(paste > clipboardEnd);
  QVERIFY(down > paste);
  QVERIFY(up > down);
  QCOMPARE(dragMoves, 2);
  QCOMPARE(packets[packets.size() - 2], QByteArray::fromHex("444d4d56014000f0"));
  QCOMPARE(peer->state(), QAbstractSocket::ConnectedState);
  stream.close();
  events.drain();
}

void OutputLoopbackTests::pausedReceiverPreservesPriorityAndBulkPacing_data()
{
  QTest::addColumn<bool>("tls");
  QTest::newRow("plain TCP") << false;
  QTest::newRow("TLS") << true;
}

void OutputLoopbackTests::pausedReceiverPreservesPriorityAndBulkPacing()
{
  QFETCH(bool, tls);
  if (tls && !QSslSocket::supportsSsl())
    QSKIP("Qt TLS backend unavailable");
  QTemporaryDir isolated;
  QVERIFY(isolated.isValid());
  Settings::setSettingsFile(isolated.filePath("settings.ini"));
  Settings::setStateFile(isolated.filePath("state.ini"));
  ProbeServer listener;
  listener.tls = tls;
  listener.setProxy(QNetworkProxy::NoProxy);
  if (tls) {
    const auto pem = isolated.filePath("probe.pem");
    deskflow::generatePemSelfSignedCert(pem);
    Settings::setValue(Settings::Security::Certificate, pem);
    QFile file(pem);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    listener.certificate = QSslCertificate(bytes, QSsl::Pem);
    listener.key = QSslKey(bytes, QSsl::Rsa, QSsl::Pem);
    QVERIFY(!listener.certificate.isNull());
    QVERIFY(!listener.key.isNull());
    FingerprintDatabase trusted;
    trusted.addTrusted({QCryptographicHash::Sha256, listener.certificate.digest(QCryptographicHash::Sha256)});
    QVERIFY(QDir().mkpath(QFileInfo(Settings::tlsTrustedServersDb()).absolutePath()));
    QVERIFY(trusted.write(Settings::tlsTrustedServersDb()));
  }
  QVERIFY(listener.listen(QHostAddress::LocalHost, 0));

  LoopbackEventQueue events;
  SocketMultiplexer multiplexer;
  TCPSocket *socket;
  if (tls) {
    auto *secure = new SecureSocket(&events, &multiplexer, IArchNetwork::AddressFamily::INet, SecurityLevel::Encrypted);
    secure->initSsl(false);
    socket = secure;
  } else {
    socket = new TCPSocket(&events, &multiplexer);
  }
  PacketStreamFilter stream(&events, socket, true);
  bool connected = false;
  bool failed = false;
  events.addHandler(
      tls ? EventTypes::DataSocketSecureConnected : EventTypes::DataSocketConnected, stream.getEventTarget(),
      [&](const auto &) { connected = true; }
  );
  events.addHandler(EventTypes::StreamOutputError, stream.getEventTarget(), [&](const auto &) { failed = true; });
  events.addHandler(EventTypes::StreamInputShutdown, stream.getEventTarget(), [&](const auto &) { failed = true; });
  NetworkAddress address("127.0.0.1", listener.serverPort());
  address.resolve();
  socket->connect(address);
  QVERIFY(waitFor(events, [&] {
    return failed || (connected &&
                      (tls ? listener.tlsPeer && listener.tlsPeer->isEncrypted() : listener.hasPendingConnections()));
  }));
  QVERIFY(!failed);
  std::unique_ptr<QTcpSocket> peer(tls ? listener.tlsPeer : listener.nextPendingConnection());
  QVERIFY(peer);
  peer->setReadBufferSize(1024);

  const std::string payload(512 * 1024, 'Z');
  const auto declaredSize = std::to_string(payload.size());
  ProtocolUtil::writef(&stream, kMsgDClipboard, 0, 7, ChunkType::DataStart, &declaredSize);
  for (size_t offset = 0; offset < payload.size(); offset += 16 * 1024) {
    const auto chunk = payload.substr(offset, 16 * 1024);
    ProtocolUtil::writef(&stream, kMsgDClipboard, 0, 7, ChunkType::DataChunk, &chunk);
  }
  const std::string empty;
  ProtocolUtil::writef(&stream, kMsgDClipboard, 0, 7, ChunkType::DataEnd, &empty);
  // Pause application reads while the real send thread can progress. Filtering
  // events still runs, reproducing a sender with a non-reading peer.
  QElapsedTimer paused;
  paused.start();
  while (paused.elapsed() < 30) {
    events.drain();
    QTest::qWait(1);
  }
  QVERIFY2(stream.hasPendingOutput(), "bulk was entirely fed into TCP/TLS during the paused receiver interval");
  ProtocolUtil::writef(&stream, kMsgDKeyDown1_1, 0x61, 0, 0x1e);
  ProtocolUtil::writef(&stream, kMsgDMouseDown, 1);
  ProtocolUtil::writef(&stream, kMsgDMouseMove, 1, 2);
  ProtocolUtil::writef(&stream, kMsgDMouseMove, 3, 4);
  ProtocolUtil::writef(&stream, kMsgDMouseUp, 1);
  ProtocolUtil::writef(&stream, kMsgDMouseWheel, 0, 120);
  ProtocolUtil::writef(&stream, kMsgDMouseRelMove, 5, 6);
  ProtocolUtil::writef(&stream, kMsgDKeyDown1_1, 0x76, KeyModifierControl, 0x2f);
  ProtocolUtil::writef(&stream, kMsgCKeepAlive);
  ProtocolUtil::writef(&stream, kMsgCLeave);

  QByteArray buffered;
  QList<QByteArray> packets;
  QVERIFY(waitFor(events, [&] {
    buffered.append(peer->readAll());
    while (buffered.size() >= 4) {
      const auto size = qFromBigEndian<quint32>(buffered.constData());
      if (size > 1024 * 1024)
        return true;
      if (buffered.size() < 4 + qsizetype(size))
        break;
      packets.append(buffered.mid(4, size));
      buffered.remove(0, 4 + size);
    }
    return failed || (!packets.empty() && packets.last() == "COUT");
  }));
  QVERIFY(!failed);
  QVERIFY(buffered.isEmpty());
  QByteArray assembled;
  QList<QByteArray> controls;
  int end = -1;
  int typing = -1;
  int heartbeat = -1;
  int paste = -1;
  for (qsizetype i = 0; i < packets.size(); ++i) {
    const auto &packet = packets[i];
    if (packet.startsWith("DCLP")) {
      if (quint8(packet[9]) == ChunkType::DataChunk)
        assembled += packet.mid(14);
      if (quint8(packet[9]) == ChunkType::DataEnd)
        end = int(i);
    } else if (packet.startsWith("CALV")) {
      heartbeat = int(i);
    } else if (packet.startsWith("DKDN") && qFromBigEndian<quint16>(packet.constData() + 6) == KeyModifierControl) {
      paste = int(i);
    } else if (packet != "COUT") {
      if (packet.startsWith("DKDN"))
        typing = int(i);
      controls.append(packet.first(4));
    }
  }
  QCOMPARE(assembled, QByteArray::fromStdString(payload));
  QVERIFY(typing >= 0 && typing < end);
  QVERIFY(heartbeat >= 0 && heartbeat < end);
  QVERIFY(paste > end);
  QCOMPARE(controls, (QList<QByteArray>{"DKDN", "DMDN", "DMMV", "DMMV", "DMUP", "DMWM", "DMRM"}));
  stream.close();
  events.drain();
}

QTEST_GUILESS_MAIN(OutputLoopbackTests)
