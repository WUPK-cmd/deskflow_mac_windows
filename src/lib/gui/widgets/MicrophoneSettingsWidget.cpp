// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "MicrophoneSettingsWidget.h"
#include "common/Settings.h"
#include "gui/config/ServerConfig.h"
#include "gui/validators/ComputerNameValidator.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <utility>

#ifdef DESKFLOW_MICROPHONE_GUI
#include <QAudioDevice>
#include <QAudioFormat>
#include <QMediaDevices>
#endif

namespace {
using Role = MicrophoneSettingsWidget::Role;

Role currentRole()
{
#ifdef Q_OS_WIN
  if (Settings::value(Settings::Core::CoreMode) == Settings::CoreMode::Server)
    return Role::WindowsServer;
#elif defined(Q_OS_MACOS)
  if (Settings::value(Settings::Core::CoreMode) == Settings::CoreMode::Client)
    return Role::MacClient;
#endif
  return Role::Unsupported;
}

QStringList targetNames(const ServerConfig &config)
{
  QStringList result;
  for (const auto &computer : config.computers()) {
    if (!computer.isNull() && !computer.isServer())
      result.append(computer.name());
  }
  result.removeDuplicates();
  return result;
}

constexpr bool featureAvailable()
{
#ifdef DESKFLOW_MICROPHONE_GUI
  return true;
#else
  return false;
#endif
}

MicrophoneSettingsWidget::DeviceCatalog discoverDevices()
{
  MicrophoneSettingsWidget::DeviceCatalog result;
#ifdef DESKFLOW_MICROPHONE_GUI
  const auto supports48k = [](const QAudioDevice &device) {
    for (const auto channels : {1, 2}) {
      for (const auto sampleFormat : {QAudioFormat::Int16, QAudioFormat::Float, QAudioFormat::Int32}) {
        QAudioFormat format;
        format.setSampleRate(48000);
        format.setChannelCount(channels);
        format.setSampleFormat(sampleFormat);
        if (device.isFormatSupported(format))
          return true;
      }
    }
    return false;
  };
  // Qt exposes endpoint names, not hardware classes. Recognize supported virtual
  // drivers and common loopback names; never offer these as physical microphones.
#ifdef Q_OS_WIN
  for (const auto &device : QMediaDevices::audioInputs())
    result.inputs.append(
        {device.description(), supports48k(device),
         MicrophoneSettingsWidget::isVirtualDeviceName(device.description(), true)}
    );
#endif
  for (const auto &device : QMediaDevices::audioOutputs())
    result.outputs.append(
        {device.description(), supports48k(device),
         MicrophoneSettingsWidget::isVirtualDeviceName(device.description(), false)}
    );
#endif
  return result;
}
} // namespace

bool MicrophoneSettingsWidget::isVirtualDeviceName(const QString &name, bool input)
{
  const QRegularExpression virtualDriver(
      QStringLiteral("cable|vb.audio|blackhole|voicemeeter|virtual|loopback"), QRegularExpression::CaseInsensitiveOption
  );
  if (virtualDriver.match(name).hasMatch())
    return true;
  if (!input)
    return false;
  const QRegularExpression loopbackInput(
      QStringLiteral("stereo mix|what u hear|monitor|立体声混音|立體聲混音"), QRegularExpression::CaseInsensitiveOption
  );
  return loopbackInput.match(name).hasMatch();
}

MicrophoneSettingsWidget::MicrophoneSettingsWidget(const ServerConfig &config, QWidget *parent)
    : MicrophoneSettingsWidget(currentRole(), featureAvailable(), targetNames(config), discoverDevices, parent)
{
}

MicrophoneSettingsWidget::MicrophoneSettingsWidget(
    Role role, bool available, const QStringList &targets, CatalogLoader catalogLoader, QWidget *parent
)
    : QWidget(parent),
      m_role(role),
      m_featureAvailable(available),
      m_catalogLoader(std::move(catalogLoader)),
      m_enabled(new QCheckBox(this)),
      m_description(new QLabel(this)),
      m_inputLabel(new QLabel(this)),
      m_outputLabel(new QLabel(this)),
      m_targetLabel(new QLabel(this)),
      m_pinLabel(new QLabel(this)),
      m_instructions(new QLabel(this)),
      m_error(new QLabel(this)),
      m_input(new QComboBox(this)),
      m_output(new QComboBox(this)),
      m_target(new QComboBox(this)),
      m_pin(new QLineEdit(this)),
      m_refresh(new QPushButton(this))
{
  setObjectName("microphoneSettings");
  m_enabled->setObjectName("microphoneEnabled");
  m_input->setObjectName("microphoneInput");
  m_output->setObjectName("microphoneOutput");
  m_target->setObjectName("microphoneTarget");
  m_pin->setObjectName("microphoneFingerprint");
  m_error->setObjectName("microphoneError");
  m_refresh->setObjectName("microphoneRefresh");
  m_description->setWordWrap(true);
  m_instructions->setWordWrap(true);
  m_instructions->setOpenExternalLinks(true);
  m_error->setWordWrap(true);
  m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_error->setAccessibleName(tr("Microphone settings error"));
  m_error->hide();
  m_target->setEditable(true);
  m_target->addItems(targets);
  for (auto combo : {m_input, m_output, m_target}) {
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(18);
  }
  m_inputLabel->setBuddy(m_input);
  m_outputLabel->setBuddy(m_output);
  m_targetLabel->setBuddy(m_target);
  m_pinLabel->setBuddy(m_pin);
  auto form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->addRow(m_inputLabel, m_input);
  form->addRow(m_outputLabel, m_output);
  form->addRow(m_targetLabel, m_target);
  form->addRow(m_pinLabel, m_pin);
  auto layout = new QVBoxLayout(this);
  layout->addWidget(m_description);
  layout->addWidget(m_enabled);
  layout->addLayout(form);
  layout->addWidget(m_refresh, 0, Qt::AlignLeft);
  layout->addWidget(m_instructions);
  layout->addWidget(m_error);
  layout->addStretch();
  connect(m_enabled, &QCheckBox::toggled, this, [this] {
    updateControls();
    Q_EMIT changed();
  });
  for (auto combo : {m_input, m_output, m_target})
    connect(combo, &QComboBox::currentTextChanged, this, &MicrophoneSettingsWidget::changed);
  for (auto combo : {m_input, m_output})
    connect(combo, &QComboBox::currentTextChanged, combo, &QComboBox::setToolTip);
  connect(m_pin, &QLineEdit::textChanged, this, &MicrophoneSettingsWidget::changed);
  connect(m_refresh, &QPushButton::clicked, this, &MicrophoneSettingsWidget::refreshDevices);
  connect(this, &MicrophoneSettingsWidget::changed, this, [this] {
    m_error->clear();
    m_error->hide();
  });
  updateText();
  loadFromConfig();
}

void MicrophoneSettingsWidget::changeEvent(QEvent *event)
{
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    updateText();
    populateDevices(m_input, m_devices.inputs, m_input->currentData().toString(), true);
    populateDevices(m_output, m_devices.outputs, m_output->currentData().toString(), false);
  }
}

void MicrophoneSettingsWidget::updateText()
{
  m_error->setAccessibleName(tr("Microphone settings error"));
  m_enabled->setText(tr("&Enable microphone following"));
  m_inputLabel->setText(tr("Physical &microphone:"));
  m_outputLabel->setText(tr("&Virtual audio output:"));
  m_targetLabel->setText(tr("Target &Mac computer:"));
  m_pinLabel->setText(tr("Mac SHA-256 &fingerprint:"));
  m_refresh->setText(tr("&Refresh devices"));
  m_refresh->setToolTip(tr("Refresh the device list without recording audio. Unavailable selections are retained."));
  m_pin->setPlaceholderText(tr("Copy the Mac's SHA-256 certificate fingerprint"));
  m_pin->setToolTip(tr("64 hexadecimal characters; spaces and colons are accepted. Verify this on the Mac."));
  m_target->setToolTip(tr("Use the exact computer name shown in Deskflow on the Mac."));
  if (!m_featureAvailable)
    m_description->setText(
        tr("This build does not include microphone following. Install a microphone-enabled build to use it.")
    );
  else if (m_role == Role::WindowsServer)
    m_description->setText(
        tr("Share this Windows microphone with your Mac as the pointer moves between computers. Audio starts only "
           "after you save and connect.")
    );
  else if (m_role == Role::MacClient)
    m_description->setText(
        tr("Receive the Windows microphone when the pointer is on this Mac. Configure the physical microphone and this "
           "Mac's identity on the Windows server.")
    );
  else
    m_description->setText(
        tr("Microphone following supports a Windows server and a Mac client. Change the computer role on the main "
           "window to configure a supported route.")
    );
  if (m_role == Role::MacClient)
    m_instructions->setText(
        tr("Install <a href=\"https://existential.audio/blackhole/\">BlackHole 2ch</a>, refresh devices, and select it "
           "here. In calling or recording apps, select BlackHole 2ch as the microphone. Enable TLS in the Security "
           "section of the Network tab. "
           "Deskflow does not install drivers or change your sound defaults.")
    );
  else
    m_instructions->setText(
        tr("Install <a href=\"https://vb-audio.com/Cable/\">VB-CABLE</a> on Windows and <a "
           "href=\"https://existential.audio/blackhole/\">BlackHole 2ch</a> on the Mac. Select CABLE Input here; "
           "select CABLE Output as the microphone in Windows apps and BlackHole 2ch in Mac apps. Enable TLS and client "
           "fingerprint verification in the Security section of the Network tab. Deskflow does not install drivers or "
           "change your sound defaults.")
    );
}

void MicrophoneSettingsWidget::updateControls()
{
  const bool windows = m_role == Role::WindowsServer;
  const bool supported = m_featureAvailable && m_role != Role::Unsupported;
  m_enabled->setEnabled(supported || m_enabled->isChecked()); // Always allow disabling an old configuration.
  const QList<QWidget *> windowsControls = {m_input, m_inputLabel, m_target, m_targetLabel, m_pin, m_pinLabel};
  for (auto control : windowsControls)
    control->setVisible(windows);
  for (auto control :
       {static_cast<QWidget *>(m_input), static_cast<QWidget *>(m_output), static_cast<QWidget *>(m_target),
        static_cast<QWidget *>(m_pin)})
    control->setEnabled(supported && m_enabled->isChecked());
  m_refresh->setEnabled(supported);
}

void MicrophoneSettingsWidget::populateDevices(
    QComboBox *combo, const QList<Device> &devices, const QString &selection, bool input
)
{
  const QSignalBlocker blocker(combo);
  combo->clear();
  combo->addItem(tr("Select a device…"), QString());
  for (const auto &device : devices) {
    if (input ? !device.virtualDevice : device.virtualDevice) {
      combo->addItem(device.name, device.name);
      combo->setItemData(combo->count() - 1, device.name, Qt::ToolTipRole);
    }
  }
  auto index = combo->findData(selection);
  if (index < 0) {
    combo->addItem(tr("%1 (unavailable or unsupported)").arg(selection), selection);
    index = combo->count() - 1;
    combo->setItemData(index, combo->itemText(index), Qt::ToolTipRole);
  }
  combo->setCurrentIndex(index);
  combo->setToolTip(combo->currentText());
}

void MicrophoneSettingsWidget::refreshDevices()
{
  const auto input = m_input->currentData().toString();
  const auto output = m_output->currentData().toString();
  m_devices = m_featureAvailable && m_catalogLoader ? m_catalogLoader() : DeviceCatalog{};
  populateDevices(m_input, m_devices.inputs, input, true);
  populateDevices(m_output, m_devices.outputs, output, false);
  Q_EMIT changed();
}

void MicrophoneSettingsWidget::load(bool defaults)
{
  const QSignalBlocker blocker(this);
  const auto value = [defaults](const QString &key) {
    return defaults ? Settings::defaultValue(key) : Settings::value(key);
  };
  m_enabled->setChecked(value(Settings::Microphone::Enabled).toBool());
  m_devices = m_featureAvailable && m_catalogLoader ? m_catalogLoader() : DeviceCatalog{};
  populateDevices(m_input, m_devices.inputs, value(Settings::Microphone::Input).toString(), true);
  populateDevices(m_output, m_devices.outputs, value(Settings::Microphone::Output).toString(), false);
  m_target->setCurrentText(value(Settings::Microphone::Target).toString());
  m_pin->setText(value(Settings::Microphone::TargetFingerprint).toString());
  m_error->clear();
  m_error->hide();
  updateControls();
}

void MicrophoneSettingsWidget::loadFromConfig()
{
  load(false);
  Q_EMIT changed();
}

void MicrophoneSettingsWidget::resetToDefault()
{
  load(true);
  Q_EMIT changed();
}

QString MicrophoneSettingsWidget::normalizedFingerprint(const QString &value)
{
  auto result = value;
  result.remove(QRegularExpression(QStringLiteral("[\\s:]")));
  return result.toLower();
}

bool MicrophoneSettingsWidget::fail(const QString &message, QWidget *control)
{
  m_error->setText(tr("Cannot save microphone settings: %1").arg(message));
  m_error->show();
  if (control)
    control->setFocus(Qt::OtherFocusReason);
  return false;
}

bool MicrophoneSettingsWidget::validateDevice(QComboBox *combo, const QList<Device> &devices, bool input)
{
  const auto name = combo->currentData().toString();
  if (name.isEmpty())
    return fail(
        input ? tr("Select a physical microphone.")
              : tr("Select a virtual audio output. Install the driver and refresh devices if it is missing."),
        combo
    );
  const Device *selected = nullptr;
  for (const auto &device : devices) {
    if (device.name == name) {
      if (selected)
        return fail(
            tr("Multiple audio devices have the same name. Give them unique names in system audio settings and refresh "
               "devices."),
            combo
        );
      selected = &device;
    }
  }
  if (!selected)
    return fail(tr("%1 is unavailable. Reconnect it or select another device, then refresh devices.").arg(name), combo);
  if (input && selected->virtualDevice)
    return fail(tr("Select a physical microphone, not a virtual or loopback recording device."), combo);
  if (!input && !selected->virtualDevice)
    return fail(tr("Select a virtual audio output such as CABLE Input or BlackHole 2ch."), combo);
  if (!selected->supports48k)
    return fail(
        tr("%1 must support 48 kHz mono or stereo audio. Check its format in system audio settings.").arg(name), combo
    );
  return true;
}

bool MicrophoneSettingsWidget::save(bool tlsEnabled, bool checkPeers)
{
  m_error->hide();
  if (m_enabled->isChecked()) {
    if (!m_featureAvailable)
      return fail(tr("Install a microphone-enabled Deskflow build, or disable microphone following."), m_enabled);
    if (m_role == Role::Unsupported)
      return fail(tr("Use a Windows server or a Mac client, or disable microphone following."), m_enabled);
    if (!tlsEnabled || (m_role == Role::WindowsServer && !checkPeers))
      return fail(
          tr("Enable TLS and, on the Windows server, client fingerprint verification in the Security section of the "
             "Network tab.")
      );
    refreshDevices();
    if (m_role == Role::WindowsServer) {
      if (!validateDevice(m_input, m_devices.inputs, true))
        return false;
      const auto target = m_target->currentText();
      if (target.trimmed().isEmpty())
        return fail(tr("Enter the target Mac's Deskflow computer name."), m_target);
      const auto targetError =
          tr("Use 1–255 letters, numbers, underscores, hyphens or dots with no spaces, matching the Mac's Deskflow "
             "computer name.");
      const validators::ComputerNameValidator nameValidator(targetError);
      if (target != target.trimmed() || !nameValidator.validate(target))
        return fail(targetError, m_target);
      if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(normalizedFingerprint(m_pin->text())).hasMatch())
        return fail(tr("Enter the Mac's 64-character hexadecimal SHA-256 certificate fingerprint."), m_pin);
    }
    if (!validateDevice(m_output, m_devices.outputs, false))
      return false;
  }
  // Hidden Windows fields on a Mac remain untouched, including existing pins.
  Settings::setValue(Settings::Microphone::Enabled, m_enabled->isChecked());
  Settings::setValue(Settings::Microphone::Output, m_output->currentData().toString());
  if (m_role == Role::WindowsServer) {
    Settings::setValue(Settings::Microphone::Input, m_input->currentData().toString());
    Settings::setValue(Settings::Microphone::Target, m_target->currentText());
    Settings::setValue(
        Settings::Microphone::TargetFingerprint,
        m_enabled->isChecked() ? normalizedFingerprint(m_pin->text()) : m_pin->text()
    );
  }
  return true;
}

bool MicrophoneSettingsWidget::isModified() const
{
  return m_enabled->isChecked() != Settings::value(Settings::Microphone::Enabled).toBool() ||
         m_input->currentData().toString() != Settings::value(Settings::Microphone::Input).toString() ||
         m_output->currentData().toString() != Settings::value(Settings::Microphone::Output).toString() ||
         m_target->currentText() != Settings::value(Settings::Microphone::Target).toString() ||
         m_pin->text() != Settings::value(Settings::Microphone::TargetFingerprint).toString();
}

bool MicrophoneSettingsWidget::isDefault() const
{
  return !m_enabled->isChecked() && m_input->currentData().toString().isEmpty() &&
         m_output->currentData().toString().isEmpty() && m_target->currentText().isEmpty() && m_pin->text().isEmpty();
}
