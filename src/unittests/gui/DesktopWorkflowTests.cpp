// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "common/I18N.h"
#include "common/Settings.h"
#include "gui/MainWindow.h"
#include "gui/dialogs/SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

class DesktopWorkflowTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
#ifndef Q_OS_MACOS
    QApplication::setStyle("fusion");
#endif
  }

  void init()
  {
    QVERIFY(m_profile.isValid());
    Settings::setSettingsFile(m_profile.filePath(QString::number(++m_case) + ".ini"));
    Settings::setStateFile(m_profile.filePath("state.ini"));
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::None);
    Settings::setValue(Settings::Core::ProcessMode, Settings::ProcessMode::Desktop);
    Settings::setValue(Settings::Core::ComputerName, "deskflow-acceptance");
    Settings::setValue(Settings::Core::Interface, "127.0.0.1");
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Gui::AutoStartCore, false);
    Settings::setValue(Settings::Gui::AutoUpdateCheck, false);
    Settings::setValue(Settings::Gui::Autohide, false);
    Settings::setValue(Settings::Gui::CloseReminder, false);
    Settings::setValue(Settings::Gui::CloseToTray, false);
    Settings::setValue(Settings::Microphone::Enabled, false);
    Settings::setValue(Settings::Microphone::Input, "Saved physical microphone");
    Settings::setValue(Settings::Microphone::Output, "Saved virtual cable");
    Settings::setValue(Settings::Microphone::Target, "saved-mac");
    Settings::setValue(Settings::Microphone::TargetFingerprint, QString(64, 'b'));
    const auto language = qEnvironmentVariable("DESKFLOW_GUI_TEST_LANGUAGE", QStringLiteral("en"));
    Settings::setValue(Settings::Core::Language, language);
    I18N::reDetectLanguages();
    I18N::setLanguage(language);
    Settings::save();
  }

  void actualMainWindowSupportsRoleAddressAndMicrophoneCancel()
  {
    // A unique GUI listener and disabled auto-start keep this real production
    // window independent of any Deskflow/Core already running on the desktop.
    MainWindow window("deskflow-workflow-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    window.open();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto send = window.findChild<QRadioButton *>("rbModeServer");
    auto receive = window.findChild<QRadioButton *>("rbModeClient");
    auto address = window.findChild<QComboBox *>("cbHostname");
    auto microphone = window.findChild<QPushButton *>("btnMicrophoneSettings");
    QVERIFY(send && receive && address && microphone);
    if (qEnvironmentVariable("DESKFLOW_GUI_TEST_LANGUAGE") == "zh_CN") {
      QVERIFY(send->text().contains(QString::fromUtf8("发送键鼠")));
      QVERIFY(microphone->text().contains(QString::fromUtf8("麦克风设置")));
    }
    QTest::mouseClick(send, Qt::LeftButton, Qt::NoModifier, QPoint(10, send->height() / 2));
    QCOMPARE(Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>(), Settings::CoreMode::Server);
    capture(window, "main-window-server.png");
    verifyRoleDescriptions(window);
    QTest::mouseClick(receive, Qt::LeftButton, Qt::NoModifier, QPoint(10, receive->height() / 2));
    QCOMPARE(Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>(), Settings::CoreMode::Client);
    QVERIFY(address->isVisible());
    QVERIFY(address->lineEdit());
    address->lineEdit()->setFocus();
    QTest::keyClicks(address->lineEdit(), "192.168.250.1");
    QCOMPARE(address->currentText(), QString("192.168.250.1"));
    capture(window, "main-window-client.png");
    verifyRoleDescriptions(window);
    QTest::mouseClick(send, Qt::LeftButton, Qt::NoModifier, QPoint(10, send->height() / 2));

    bool openedMicrophoneTab = false;
    bool edited = false;
    QTimer action;
    action.setInterval(20);
    connect(&action, &QTimer::timeout, &window, [&] {
      auto dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      action.stop();
      auto tabs = dialog->findChild<QTabWidget *>("tabWidget");
      auto target = dialog->findChild<QComboBox *>("microphoneTarget");
      auto buttons = dialog->findChild<QDialogButtonBox *>();
      openedMicrophoneTab = tabs && tabs->currentWidget()->objectName() == "microphoneSettings";
      if (target) {
        // Editing the fixture through the actual editor must not survive Cancel.
        target->setEnabled(true);
        target->lineEdit()->setFocus();
        target->lineEdit()->selectAll();
        QTest::keyClicks(target->lineEdit(), "unsaved-mac");
        edited = target->currentText() == "unsaved-mac";
      }
      capture(*dialog, "microphone-preferences.png");
      if (buttons)
        QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
      else
        dialog->reject();
    });
    action.start();
    QTest::mouseClick(microphone, Qt::LeftButton);
    QVERIFY(openedMicrophoneTab);
    QVERIFY(edited);
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("saved-mac"));
    QVERIFY(!Settings::value(Settings::Microphone::Enabled).toBool());
    auto log = window.findChild<QDockWidget *>();
    QVERIFY(log);
    log->show();
    for (const auto width : {540, 700, 900}) {
      window.resize(width, 700);
      verifyRoleDescriptions(window);
    }
    window.close();
  }

  void actualMainWindowSavePersistsWithoutStartingCore()
  {
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    MainWindow window("deskflow-workflow-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    window.open();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    bool saved = false;
    QTimer action;
    action.setInterval(20);
    connect(&action, &QTimer::timeout, &window, [&] {
      auto dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      action.stop();
      auto buttons = dialog->findChild<QDialogButtonBox *>();
      auto title = dialog->findChild<QCheckBox *>("cbShowVersion");
      if (buttons && title) {
        // A normal Preferences change exercises MainWindow's real save/apply path.
        title->setChecked(!title->isChecked());
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        saved = dialog->result() == QDialog::Accepted;
      }
      if (!saved)
        dialog->reject();
    });
    auto microphone = window.findChild<QPushButton *>("btnMicrophoneSettings");
    QVERIFY(microphone);
    action.start();
    QTest::mouseClick(microphone, Qt::LeftButton);
    QVERIFY(saved);
    QSettings disk(Settings::settingsFile(), QSettings::IniFormat);
    QVERIFY(disk.contains(Settings::Gui::ShowVersionInTitle));
    QCOMPARE(disk.value(Settings::Microphone::Input).toString(), QString("Saved physical microphone"));
    QCOMPARE(disk.value(Settings::Microphone::Output).toString(), QString("Saved virtual cable"));
    QCOMPARE(disk.value(Settings::Microphone::TargetFingerprint).toString(), QString(64, 'b'));
    QVERIFY(!Settings::value(Settings::Gui::AutoStartCore).toBool());
    window.close();
  }

private:
  static void verifyRoleDescriptions(MainWindow &window)
  {
    for (const auto name : {"lblServerDescription", "lblClientDescription"}) {
      const auto label = window.findChild<QLabel *>(name);
      QVERIFY(label);
      QApplication::processEvents();
      QTest::qWait(50);
      QVERIFY2(
          label->height() >= label->heightForWidth(label->width()),
          qPrintable(QString("%1 has height %2, text requires %3 at width %4")
                         .arg(name)
                         .arg(label->height())
                         .arg(label->heightForWidth(label->width()))
                         .arg(label->width()))
      );
    }
  }

  static void capture(QWidget &window, const QString &name)
  {
    const auto directory = qEnvironmentVariable("DESKFLOW_GUI_EVIDENCE_DIR");
    if (directory.isEmpty())
      return;
    QDir().mkpath(directory);
    QApplication::processEvents();
    QTest::qWait(200); // Let native layout changes and control animations settle.
    QVERIFY(window.grab().save(QDir(directory).filePath(name)));
  }

  QTemporaryDir m_profile;
  int m_case = 0;
};

QTEST_MAIN(DesktopWorkflowTests)
#include "DesktopWorkflowTests.moc"
