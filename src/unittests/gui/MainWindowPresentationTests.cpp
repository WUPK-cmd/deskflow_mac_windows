// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "gui/MainWindow.h"
#include "ui_MainWindow.h"

#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QtTest>

// Render the production form without constructing MainWindow: its constructor
// owns the shared GUI IPC endpoint and can attach to the installed Core.
class MainWindowPresentationTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase()
  {
#ifndef Q_OS_MACOS
    QApplication::setStyle(QStringLiteral("fusion"));
#endif
  }

  void freshProfilesStartWithTheLogCollapsed()
  {
    QVERIFY(!Settings::defaultValue(Settings::Gui::LogExpanded).toBool());
  }

  void listeningDoesNotClaimAConnectedPeer()
  {
    const auto status = MainWindow::connectionStatusText(
        Settings::CoreMode::Server, deskflow::core::ProcessState::Started, deskflow::core::ConnectionState::Listening,
        {}
    );
    QCOMPARE(status.first, QString("Waiting for computers"));
    QVERIFY(status.second.contains("Receive"));
    QVERIFY(!status.first.contains("Connected"));
  }

  void connectedClientIdentifiesTheActualRemoteHost()
  {
    const auto status = MainWindow::connectionStatusText(
        Settings::CoreMode::Client, deskflow::core::ProcessState::Started, deskflow::core::ConnectionState::Connected,
        "office-server"
    );
    QCOMPARE(status.first, QString("Connected"));
    QVERIFY(status.second.contains("office-server"));
  }

  void stoppedAndRetryStatesOfferAnAction()
  {
    using deskflow::core::ConnectionState;
    using deskflow::core::ProcessState;
    auto status = MainWindow::connectionStatusText(
        Settings::CoreMode::None, ProcessState::Stopped, ConnectionState::Disconnected, {}
    );
    QCOMPARE(status.first, QString("Ready to set up"));
    QVERIFY(status.second.contains("Choose"));
    status = MainWindow::connectionStatusText(
        Settings::CoreMode::Client, ProcessState::Stopped, ConnectionState::Disconnected, {}
    );
    QVERIFY(status.second.contains("address"));
    status = MainWindow::connectionStatusText(
        Settings::CoreMode::Client, ProcessState::RetryPending, ConnectionState::Disconnected, "office-server"
    );
    QCOMPARE(status.first, QString("Waiting to retry"));
    QVERIFY(status.second.contains("address"));
    // A stale connection report must never override a stopped process.
    status = MainWindow::connectionStatusText(
        Settings::CoreMode::Client, ProcessState::Stopped, ConnectionState::Connected, "office-server"
    );
    QCOMPARE(status.first, QString("Not connected"));
  }

  void firstRunExplainsTheNextStep()
  {
    QMainWindow window;
    Ui::MainWindow form;
    form.setupUi(&window);
    const auto heading = window.findChild<QLabel *>("lblConnectionTitle");
    const auto detail = window.findChild<QLabel *>("lblConnectionDetail");
    QVERIFY2(heading, "A visible connection summary must lead the main window");
    QVERIFY(detail);
    QCOMPARE(heading->text(), QString("Ready to set up"));
    QVERIFY(detail->text().contains("keyboard and mouse"));
    QVERIFY(detail->wordWrap());
  }

  void rolesUsePlainLanguageAndKeyboardNavigation()
  {
    QMainWindow window;
    Ui::MainWindow form;
    form.setupUi(&window);
    QVERIFY(QString(form.rbModeServer->text()).remove('&').contains("Send"));
    QVERIFY(QString(form.rbModeClient->text()).remove('&').contains("Receive"));
    QVERIFY(form.rbModeServer->text().contains('&'));
    QVERIFY(form.rbModeClient->text().contains('&'));
    form.rbModeServer->setChecked(true);
    form.rbModeClient->setChecked(true);
    QVERIFY(!form.rbModeServer->isChecked());
    QVERIFY(form.rbModeClient->isChecked());
  }

  void microphoneSettingsHasADirectKeyboardAccessibleEntry()
  {
    QMainWindow window;
    Ui::MainWindow form;
    form.setupUi(&window);
    const auto microphone = window.findChild<QPushButton *>("btnMicrophoneSettings");
    QVERIFY2(microphone, "Microphone preferences must be discoverable from the main window");
    QVERIFY(microphone->text().contains("Microphone"));
    QVERIFY(microphone->text().contains('&'));
    QVERIFY(microphone->focusPolicy() & Qt::TabFocus);
    QVERIFY(!microphone->toolTip().isEmpty());
  }

  void layoutKeepsPrimaryControlsInsideACompactWindow()
  {
    QMainWindow window;
    Ui::MainWindow form;
    form.setupUi(&window);
    form.serverOptions->hide();
    form.lineEditName->hide();
    form.lblIpAddresses->hide();
    window.resize(700, 620);
    window.show();
    QCoreApplication::processEvents();
    for (auto *control :
         {static_cast<QWidget *>(form.rbModeServer), static_cast<QWidget *>(form.rbModeClient),
          static_cast<QWidget *>(form.btnToggleCore), static_cast<QWidget *>(form.btnMicrophoneSettings)}) {
      const auto bounds = QRect(control->mapTo(&window, QPoint()), control->size());
      QVERIFY2(window.rect().contains(bounds), qPrintable(control->objectName()));
    }
    QVERIFY(window.maximumWidth() > window.width());
    QVERIFY(window.maximumHeight() > window.height());
  }

  void roleDescriptionsRemainFullyVisibleAfterModeChangesAndResize()
  {
    QMainWindow window;
    Ui::MainWindow form;
    form.setupUi(&window);
    form.lineEditName->hide();
    form.lblNoMode->hide();
    window.show();
    for (const auto client : {false, true}) {
      form.serverOptions->setVisible(!client);
      form.clientOptions->setVisible(client);
      for (const auto width : {540, 700, 900}) {
        window.resize(width, 560);
        QTest::qWait(50);
        for (const auto name : {"lblServerDescription", "lblClientDescription"}) {
          const auto description = window.findChild<QLabel *>(name);
          QVERIFY(description);
          const auto required = description->heightForWidth(description->width());
          QVERIFY2(
              description->height() >= required, qPrintable(QString("%1: allocated height %2, required %3 at width %4")
                                                                .arg(name)
                                                                .arg(description->height())
                                                                .arg(required)
                                                                .arg(description->width()))
          );
        }
      }
    }
  }
};

QTEST_MAIN(MainWindowPresentationTests)
#include "MainWindowPresentationTests.moc"
