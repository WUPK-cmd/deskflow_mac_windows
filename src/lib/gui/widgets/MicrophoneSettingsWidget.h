// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#pragma once

#include <QList>
#include <QStringList>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class ServerConfig;

class MicrophoneSettingsWidget : public QWidget
{
  Q_OBJECT
public:
  enum class Role
  {
    WindowsServer,
    MacClient,
    Unsupported
  };
  struct Device
  {
    QString name;
    bool supports48k = false;
    bool virtualDevice = false;
  };
  struct DeviceCatalog
  {
    QList<Device> inputs;
    QList<Device> outputs;
  };
  using CatalogLoader = std::function<DeviceCatalog()>;

  explicit MicrophoneSettingsWidget(const ServerConfig &config, QWidget *parent = nullptr);
  // Explicit context and catalog keep tests independent of hardware and audio permissions.
  MicrophoneSettingsWidget(
      Role role, bool featureAvailable, const QStringList &targets, CatalogLoader catalogLoader,
      QWidget *parent = nullptr
  );

  void loadFromConfig();
  void resetToDefault();
  bool save(bool tlsEnabled, bool checkPeers);
  bool isModified() const;
  bool isDefault() const;
  void refreshDevices();
  static bool isVirtualDeviceName(const QString &name, bool input);

Q_SIGNALS:
  void changed();

protected:
  void changeEvent(QEvent *event) override;

private:
  void load(bool defaults);
  void updateText();
  void updateControls();
  bool fail(const QString &message, QWidget *control = nullptr);
  bool validateDevice(QComboBox *combo, const QList<Device> &devices, bool input);
  void populateDevices(QComboBox *combo, const QList<Device> &devices, const QString &selection, bool input);
  static QString normalizedFingerprint(const QString &value);

  Role m_role;
  bool m_featureAvailable;
  CatalogLoader m_catalogLoader;
  DeviceCatalog m_devices;
  QCheckBox *m_enabled;
  QLabel *m_description;
  QLabel *m_inputLabel;
  QLabel *m_outputLabel;
  QLabel *m_targetLabel;
  QLabel *m_pinLabel;
  QLabel *m_instructions;
  QLabel *m_error;
  QComboBox *m_input;
  QComboBox *m_output;
  QComboBox *m_target;
  QLineEdit *m_pin;
  QPushButton *m_refresh;
};
