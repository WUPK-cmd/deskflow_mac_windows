/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

// important: this is used for settings paths on some platforms,
// and must not be a url. qt automatically converts this to reverse domain
// notation (rdn), e.g. org.deskflow
const auto kOrgDomain = QStringLiteral("deskflow.org");

const auto kUrlSourceQuery = QStringLiteral("source=gui");
const auto kUrlApp = QStringLiteral("https://github.com/WUPK-cmd/deskflow_mac_windows");
const auto kUrlHelp = QStringLiteral("%1/issues").arg(kUrlApp);
const auto kUrlDownload = QStringLiteral("%1/releases").arg(kUrlApp);
const auto kUrlWiki = QStringLiteral("%1/blob/main/docs/Readme.md").arg(kUrlApp);
const auto kUrlUpdateCheck =
    QStringLiteral("https://api.github.com/repos/WUPK-cmd/deskflow_mac_windows/releases/latest");

#if defined(Q_OS_LINUX)
const auto kUrlGnomeTrayFix = QStringLiteral("https://extensions.gnome.org/extension/615/appindicator-support/");
#endif
