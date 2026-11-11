/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class InputStateTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void wheel_smallPositiveDeltas_retainRemainder();
  void wheel_negativeDeltas_retainRemainder();
  void wheel_directionChange_cancelsRemainder();
  void wheel_axes_keepIndependentRemainders();
  void wheel_fractionalScale_retainsSubLineAndSubUnitValues();
  void wheel_invertedScale_preservesDirection();
  void wheel_reset_discardsOldSessionRemainder();
  void modifier_overlappingSides_remainHeld_data();
  void modifier_overlappingSides_remainHeld();
  void modifier_repeatedPress_doesNotRequireExtraRelease();
  void buttons_releaseAll_emitsEverySyntheticRelease();
  void buttons_releaseAll_emptyLedger_emitsNothing();
  void buttons_releaseAll_isIdempotent();
  void buttons_invalidIndex_isIgnored();
  void windowsAsyncState_usesHighBit_data();
  void windowsAsyncState_usesHighBit();
  void windowsHook_localKeyUp_overridesPreEventAsyncState();
  void windowsHook_localDownAndRepeat_keepCurrentTransition();
  void windowsHook_suppressedHeldKey_survivesRefresh_data();
  void windowsHook_suppressedHeldKey_survivesRefresh();
  void windowsHook_suppressedRelease_isNotResurrected();
  void windowsHook_modifierAliases_followResultingSides_data();
  void windowsHook_modifierAliases_followResultingSides();
  void windowsHook_reset_discardsOldState();
  void windowsHook_localTransition_resumesAsyncRecovery();
  void windowsHook_firstLocalDown_seedsPreHeldControl();
  void windowsHook_firstRelayDown_seedsPreHeldModifiers();
};
