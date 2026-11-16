// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "common/Settings.h"
#include "gui/TlsUtility.h"
#include "gui/dialogs/SettingsDialog.h"
#include "gui/widgets/MicrophoneSettingsWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>

class MicrophoneSettingsTests : public QObject
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
    QVERIFY(m_settings.isValid());
    Settings::setSettingsFile(m_settings.filePath(QString::number(++m_counter) + ".ini"));
    Settings::setStateFile(m_settings.filePath("state.ini"));
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Core::Language, "en");
    Settings::setValue(Settings::Security::TlsEnabled, true);
    Settings::setValue(Settings::Security::CheckPeers, true);
    Settings::setValue(Settings::Microphone::Enabled, false);
    Settings::setValue(Settings::Microphone::Input, "Saved microphone");
    Settings::setValue(Settings::Microphone::Output, "Saved virtual cable");
    Settings::setValue(Settings::Microphone::Target, "Mac-office");
    Settings::setValue(Settings::Microphone::TargetFingerprint, QString(64, 'a'));
  }

  void preferencesExposeOptInAndRetainMissingDevices()
  {
    const ServerConfig config;
    SettingsDialog dialog(nullptr, config);
    const auto enabled = dialog.findChild<QCheckBox *>("microphoneEnabled");
    QVERIFY2(enabled, "Preferences must expose the explicit microphone opt-in");
    QVERIFY(!enabled->isChecked());
    const auto input = dialog.findChild<QComboBox *>("microphoneInput");
    const auto output = dialog.findChild<QComboBox *>("microphoneOutput");
    QVERIFY(input);
    QVERIFY(output);
    QCOMPARE(input->currentData().toString(), QString("Saved microphone"));
    QCOMPARE(output->currentData().toString(), QString("Saved virtual cable"));
  }

  void cancelDoesNotSaveMicrophoneEdits()
  {
    const ServerConfig config;
    SettingsDialog dialog(nullptr, config);
    const auto target = dialog.findChild<QComboBox *>("microphoneTarget");
    QVERIFY(target);
    target->setCurrentText("Changed-Mac");
    dialog.reject();
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-office"));
  }

  void savingDisabledPreservesRouting()
  {
    const ServerConfig config;
    SettingsDialog dialog(nullptr, config);
    const auto enabled = dialog.findChild<QCheckBox *>("microphoneEnabled");
    QVERIFY(enabled);
    enabled->setChecked(false);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection));
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(Settings::value(Settings::Microphone::Input).toString(), QString("Saved microphone"));
    QCOMPARE(Settings::value(Settings::Microphone::TargetFingerprint).toString(), QString(64, 'a'));
  }

  void serverSaveNormalizesFingerprintAndPersists()
  {
    const ServerConfig config;
    auto widget = serverWidget();
    SettingsDialog dialog(nullptr, config, widget);
    configureValidServer(widget);
    widget->findChild<QComboBox *>("microphoneTarget")->setCurrentText("Mac-study");
    widget->findChild<QLineEdit *>("microphoneFingerprint")->setText(QString("AB:").repeated(31) + "AB");
    QVERIFY(QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection));
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), true);
    QCOMPARE(Settings::value(Settings::Microphone::Input).toString(), QString("Desk microphone"));
    QCOMPARE(Settings::value(Settings::Microphone::Output).toString(), QString("CABLE Input"));
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-study"));
    QCOMPARE(Settings::value(Settings::Microphone::TargetFingerprint).toString(), QString("ab").repeated(32));
    MicrophoneSettingsWidget reloaded(MicrophoneSettingsWidget::Role::WindowsServer, true, {}, catalog);
    QVERIFY(!reloaded.isModified());
    QCOMPARE(reloaded.findChild<QComboBox *>("microphoneTarget")->currentText(), QString("Mac-study"));
  }

  void invalidConfigurationDoesNotChangeAnySettings_data()
  {
    QTest::addColumn<QString>("invalid");
    for (const auto value : {"pin", "target", "input", "output", "format", "loopback", "tls", "peers", "build", "role"})
      QTest::newRow(value) << QString::fromLatin1(value);
  }

  void invalidConfigurationDoesNotChangeAnySettings()
  {
    QFETCH(QString, invalid);
    const ServerConfig config;
    auto devices = catalog();
    if (invalid == "format")
      devices.inputs[0].supports48k = false;
    if (invalid == "loopback")
      devices.inputs[0].virtualDevice = true;
    const auto role =
        invalid == "role" ? MicrophoneSettingsWidget::Role::Unsupported : MicrophoneSettingsWidget::Role::WindowsServer;
    auto widget = new MicrophoneSettingsWidget(role, invalid != "build", {}, [devices] { return devices; });
    SettingsDialog dialog(nullptr, config, widget);
    configureValidServer(widget);
    if (invalid == "pin")
      widget->findChild<QLineEdit *>("microphoneFingerprint")->setText(QString(64, 'z'));
    if (invalid == "target")
      widget->findChild<QComboBox *>("microphoneTarget")->setCurrentText(" ");
    if (invalid == "input")
      widget->findChild<QComboBox *>("microphoneInput")->setCurrentIndex(0);
    if (invalid == "output")
      widget->findChild<QComboBox *>("microphoneOutput")->setCurrentIndex(0);
    if (invalid == "tls")
      dialog.findChild<QGroupBox *>("groupSecurity")->setChecked(false);
    if (invalid == "peers")
      dialog.findChild<QCheckBox *>("cbRequireClientCert")->setChecked(false);
    const auto oldPort = Settings::value(Settings::Core::Port);
    const auto oldKeySize = Settings::value(Settings::Security::KeySize);
    dialog.findChild<QSpinBox *>("sbPort")->setValue(oldPort.toInt() + 1);
    dialog.findChild<QComboBox *>("comboTlsKeyLength")->setCurrentText("4096");
    QVERIFY(QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection));
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(!widget->findChild<QLabel *>("microphoneError")->text().isEmpty());
    QCOMPARE(dialog.findChild<QTabWidget *>("tabWidget")->currentWidget(), widget);
    QCOMPARE(Settings::value(Settings::Core::Port), oldPort);
    QCOMPARE(Settings::value(Settings::Security::KeySize), oldKeySize);
    QCOMPARE(Settings::value(Settings::Security::TlsEnabled).toBool(), true);
    QCOMPARE(Settings::value(Settings::Security::CheckPeers).toBool(), true);
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), false);
    QCOMPARE(Settings::value(Settings::Microphone::Input).toString(), QString("Saved microphone"));
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-office"));
    QCOMPARE(Settings::value(Settings::Microphone::TargetFingerprint).toString(), QString(64, 'a'));
  }

  void refreshRetainsDisappearedSelectionWithoutFallback()
  {
    auto devices = catalog();
    MicrophoneSettingsWidget widget(MicrophoneSettingsWidget::Role::WindowsServer, true, {}, [&devices] {
      return devices;
    });
    configureValidServer(&widget);
    devices.inputs = {{"Other microphone", true, false}};
    widget.findChild<QPushButton *>("microphoneRefresh")->click();
    const auto input = widget.findChild<QComboBox *>("microphoneInput");
    QCOMPARE(input->currentData().toString(), QString("Desk microphone"));
    QVERIFY(input->currentText().contains("unavailable"));
    QVERIFY(!widget.save(true, true));
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), false);
  }

  void invalidTargetSyntaxIsRejected_data()
  {
    QTest::addColumn<QString>("target");
    QTest::newRow("spaces") << QString("Mac studio");
    QTest::newRow("punctuation") << QString("Mac-studio!");
    QTest::newRow("too-long") << QString(256, 'm');
    QTest::newRow("leading-space") << QString(" Mac-studio");
    QTest::newRow("trailing-space") << QString("Mac-studio ");
    QTest::newRow("trailing-newline") << QString("Mac-studio\n");
  }

  void invalidTargetSyntaxIsRejected()
  {
    QFETCH(QString, target);
    const ServerConfig config;
    auto widget = serverWidget();
    SettingsDialog dialog(nullptr, config, widget);
    configureValidServer(widget);
    widget->findChild<QComboBox *>("microphoneTarget")->setCurrentText(target);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection));
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(!widget->findChild<QLabel *>("microphoneError")->text().isEmpty());
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), false);
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-office"));
  }

  void disabledLegacyTargetIsPreservedExactly()
  {
    const auto legacyTarget = QString("  Old Mac?  ");
    Settings::setValue(Settings::Microphone::Target, legacyTarget);
    const ServerConfig config;
    SettingsDialog dialog(nullptr, config, serverWidget());
    QVERIFY(QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection));
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), false);
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), legacyTarget);
  }

  void longestValidTargetIsAcceptedExactly()
  {
    const auto target = QString("Mac_") + QString(249, 'a') + ".1";
    QCOMPARE(target.size(), 255);
    auto widget = std::unique_ptr<MicrophoneSettingsWidget>(serverWidget());
    configureValidServer(widget.get());
    widget->findChild<QComboBox *>("microphoneTarget")->setCurrentText(target);
    QVERIFY(widget->save(true, true));
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), target);
  }

  void macUsesOnlyOutputAndPreservesWindowsIdentity()
  {
    const ServerConfig config;
    auto widget = new MicrophoneSettingsWidget(MicrophoneSettingsWidget::Role::MacClient, true, {}, catalog);
    SettingsDialog dialog(nullptr, config, widget);
    QVERIFY(widget->findChild<QComboBox *>("microphoneInput")->isHidden());
    QVERIFY(widget->findChild<QComboBox *>("microphoneTarget")->isHidden());
    QVERIFY(widget->findChild<QLineEdit *>("microphoneFingerprint")->isHidden());
    widget->findChild<QCheckBox *>("microphoneEnabled")->setChecked(true);
    auto output = widget->findChild<QComboBox *>("microphoneOutput");
    output->setCurrentIndex(output->findData("BlackHole 2ch"));
    QVERIFY(QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection));
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(Settings::value(Settings::Microphone::Output).toString(), QString("BlackHole 2ch"));
    QCOMPARE(Settings::value(Settings::Microphone::Input).toString(), QString("Saved microphone"));
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-office"));
    QCOMPARE(Settings::value(Settings::Microphone::TargetFingerprint).toString(), QString(64, 'a'));
  }

  void resetAndDefaultsParticipateInDialogButtons()
  {
    const ServerConfig config;
    auto widget = serverWidget();
    SettingsDialog dialog(nullptr, config, widget);
    auto buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QVERIFY(!widget->isModified());
    configureValidServer(widget);
    QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
    buttons->button(QDialogButtonBox::Reset)->click();
    QVERIFY(!widget->isModified());
    QCOMPARE(widget->findChild<QComboBox *>("microphoneInput")->currentData().toString(), QString("Saved microphone"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->click();
    QVERIFY(widget->isDefault());
    QVERIFY(widget->isModified());
    dialog.reject();
    QCOMPARE(Settings::value(Settings::Microphone::Input).toString(), QString("Saved microphone"));
  }

  void unsupportedContextCanDisableSavedConfiguration()
  {
    Settings::setValue(Settings::Microphone::Enabled, true);
    MicrophoneSettingsWidget widget(MicrophoneSettingsWidget::Role::Unsupported, false, {}, catalog);
    const auto enabled = widget.findChild<QCheckBox *>("microphoneEnabled");
    QVERIFY(enabled->isEnabled());
    enabled->setChecked(false);
    QVERIFY(widget.save(false, false));
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), false);
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-office"));
  }

  void endpointNameClassificationKeepsPhysicalOutputsOut()
  {
    QVERIFY(MicrophoneSettingsWidget::isVirtualDeviceName("CABLE Input (VB-Audio Virtual Cable)", false));
    QVERIFY(MicrophoneSettingsWidget::isVirtualDeviceName("BlackHole 2ch", false));
    QVERIFY(!MicrophoneSettingsWidget::isVirtualDeviceName("Monitor (NVIDIA High Definition Audio)", false));
    QVERIFY(!MicrophoneSettingsWidget::isVirtualDeviceName("Speakers (Realtek Audio)", false));
    QVERIFY(!MicrophoneSettingsWidget::isVirtualDeviceName("Desk microphone", true));
    QVERIFY(MicrophoneSettingsWidget::isVirtualDeviceName("Stereo Mix (Realtek Audio)", true));
    QVERIFY(MicrophoneSettingsWidget::isVirtualDeviceName("立体声混音", true));
    QVERIFY(MicrophoneSettingsWidget::isVirtualDeviceName("CABLE Output", true));
  }

  void certificateRegenerationUsesPendingKeySizeWithoutSavingIt()
  {
    // Both the certificate and configuration are inside this fixture's temporary directory.
    const auto certificate = m_settings.filePath("fixture-certificate.pem");
    Settings::setValue(Settings::Security::Certificate, certificate);
    Settings::setValue(Settings::Security::KeySize, 2048);
    const ServerConfig config;
    SettingsDialog dialog(nullptr, config, serverWidget());
    dialog.findChild<QComboBox *>("comboTlsKeyLength")->setCurrentText("4096");
    QCOMPARE(Settings::value(Settings::Security::KeySize).toInt(), 2048);
    QTimer::singleShot(0, &dialog, [] {
      for (const auto topLevel : QApplication::topLevelWidgets()) {
        if (auto message = qobject_cast<QMessageBox *>(topLevel))
          message->accept();
      }
    });
    dialog.findChild<QPushButton *>("btnTlsRegenCert")->click();
    QCOMPARE(deskflow::gui::TlsUtility::getCertKeyLength(certificate), 4096);
    QCOMPARE(Settings::value(Settings::Security::KeySize).toInt(), 2048);
    dialog.reject();
  }

  void nativeClickTypeSaveAndCancel()
  {
    const ServerConfig config;
    SettingsDialog dialog(nullptr, config, serverWidget());
    dialog.showMicrophoneTab();
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto enabled = dialog.findChild<QCheckBox *>("microphoneEnabled");
    QTest::mouseClick(enabled, Qt::LeftButton, Qt::NoModifier, QPoint(10, enabled->height() / 2));
    QVERIFY(enabled->isChecked());
    auto buttons = dialog.findChild<QDialogButtonBox *>();
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(dialog.isVisible());
    QVERIFY(!dialog.findChild<QLabel *>("microphoneError")->isHidden());
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), false);
    capture(dialog, "microphone-validation-error.png");
    for (auto name : {"microphoneInput", "microphoneOutput"}) {
      auto combo = dialog.findChild<QComboBox *>(name);
      combo->setFocus();
      QTest::keyClick(combo, Qt::Key_Home);
      QTest::keyClick(combo, Qt::Key_Down);
    }
    auto target = dialog.findChild<QComboBox *>("microphoneTarget")->lineEdit();
    target->setFocus();
    QTest::keySequence(target, QKeySequence(QKeySequence::SelectAll));
    QTest::keyClicks(target, "Mac-studio");
    auto pin = dialog.findChild<QLineEdit *>("microphoneFingerprint");
    pin->setFocus();
    QTest::keySequence(pin, QKeySequence(QKeySequence::SelectAll));
    QTest::keyClicks(pin, QString(64, 'B'));
    QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
    capture(dialog, "microphone-configured.png");
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(Settings::value(Settings::Microphone::Enabled).toBool(), true);
    QCOMPARE(Settings::value(Settings::Microphone::Input).toString(), QString("Desk microphone"));
    QCOMPARE(Settings::value(Settings::Microphone::Output).toString(), QString("CABLE Input"));
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-studio"));
    QCOMPARE(Settings::value(Settings::Microphone::TargetFingerprint).toString(), QString(64, 'b'));

    SettingsDialog cancelled(nullptr, config, serverWidget());
    cancelled.showMicrophoneTab();
    cancelled.show();
    QVERIFY(QTest::qWaitForWindowExposed(&cancelled));
    auto cancelledTarget = cancelled.findChild<QComboBox *>("microphoneTarget")->lineEdit();
    cancelledTarget->setFocus();
    QTest::keySequence(cancelledTarget, QKeySequence(QKeySequence::SelectAll));
    QTest::keyClicks(cancelledTarget, "Do-not-save-this");
    capture(cancelled, "microphone-before-cancel.png");
    QTest::mouseClick(cancelled.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QCOMPARE(cancelled.result(), int(QDialog::Rejected));
    QCOMPARE(Settings::value(Settings::Microphone::Target).toString(), QString("Mac-studio"));
  }

private:
  static void capture(QWidget &widget, const QString &name)
  {
    const auto directory = qEnvironmentVariable("DESKFLOW_GUI_EVIDENCE_DIR");
    if (directory.isEmpty())
      return;
    QTest::qWait(200);
    QVERIFY(QDir().mkpath(directory));
    QVERIFY(widget.grab().save(QDir(directory).filePath(name)));
  }

  static MicrophoneSettingsWidget::DeviceCatalog catalog()
  {
    return {
        {{"Desk microphone", true, false}, {"CABLE Output", true, true}},
        {{"CABLE Input", true, true}, {"BlackHole 2ch", true, true}, {"Speakers", true, false}}
    };
  }

  static MicrophoneSettingsWidget *serverWidget()
  {
    return new MicrophoneSettingsWidget(MicrophoneSettingsWidget::Role::WindowsServer, true, {"Mac-office"}, catalog);
  }

  static void configureValidServer(MicrophoneSettingsWidget *widget)
  {
    widget->findChild<QCheckBox *>("microphoneEnabled")->setChecked(true);
    const auto input = widget->findChild<QComboBox *>("microphoneInput");
    input->setCurrentIndex(input->findData("Desk microphone"));
    const auto output = widget->findChild<QComboBox *>("microphoneOutput");
    output->setCurrentIndex(output->findData("CABLE Input"));
  }

  QTemporaryDir m_settings;
  int m_counter = 0;
};

QTEST_MAIN(MicrophoneSettingsTests)
#include "MicrophoneSettingsTests.moc"
