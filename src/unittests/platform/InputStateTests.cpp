/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InputStateTests.h"
#include "platform/InputState.h"

#include <QTest>

#include <algorithm>
#include <vector>

using namespace deskflow::platform;

void InputStateTests::wheel_smallPositiveDeltas_retainRemainder()
{
  WheelAccumulator wheel;
  QCOMPARE(wheel.add(30), 0);
  QCOMPARE(wheel.add(30), 0);
  QCOMPARE(wheel.add(30), 0);
  QCOMPARE(wheel.add(30), 1);
  QCOMPARE(wheel.add(120), 1);
}

void InputStateTests::wheel_negativeDeltas_retainRemainder()
{
  WheelAccumulator wheel;
  QCOMPARE(wheel.add(-60), 0);
  QCOMPARE(wheel.add(-60), -1);
  QCOMPARE(wheel.add(-270), -2);
  QCOMPARE(wheel.add(-90), -1);
}

void InputStateTests::wheel_directionChange_cancelsRemainder()
{
  WheelAccumulator wheel;
  QCOMPARE(wheel.add(90), 0);
  QCOMPARE(wheel.add(-90), 0);
  QCOMPARE(wheel.add(-90), 0);
  QCOMPARE(wheel.add(-30), -1);
}

void InputStateTests::wheel_axes_keepIndependentRemainders()
{
  WheelAccumulator x;
  WheelAccumulator y;
  QCOMPARE(x.add(60), 0);
  QCOMPARE(y.add(-30), 0);
  QCOMPARE(x.add(60), 1);
  QCOMPARE(y.add(-90), -1);
}

void InputStateTests::wheel_fractionalScale_retainsSubLineAndSubUnitValues()
{
  WheelAccumulator wheel;
  QCOMPARE(wheel.add(120, 0.5), 0);
  QCOMPARE(wheel.add(120, 0.5), 1);
  int total = 0;
  for (int i = 0; i < 1200; ++i) {
    total += wheel.add(1, 0.1);
  }
  QCOMPARE(total, 1);
  wheel.reset();
  total = 0;
  for (int i = 0; i < 10; ++i) {
    total += wheel.add(120, 0.3);
  }
  QCOMPARE(total, 3);
}

void InputStateTests::wheel_invertedScale_preservesDirection()
{
  WheelAccumulator wheel;
  QCOMPARE(wheel.add(60, -1.5), 0);
  QCOMPARE(wheel.add(60, -1.5), -1);
  QCOMPARE(wheel.add(120, -1.5), -2);
}

void InputStateTests::wheel_reset_discardsOldSessionRemainder()
{
  WheelAccumulator wheel;
  QCOMPARE(wheel.add(90), 0);
  wheel.reset();
  QCOMPARE(wheel.add(30), 0);
  QCOMPARE(wheel.add(90), 1);
}

void InputStateTests::modifier_overlappingSides_remainHeld_data()
{
  QTest::addColumn<bool>("firstRight");
  QTest::newRow("left-first") << false;
  QTest::newRow("right-first") << true;
}

void InputStateTests::modifier_overlappingSides_remainHeld()
{
  QFETCH(bool, firstRight);
  ModifierKeyState modifier;
  modifier.set(firstRight, true);
  modifier.set(!firstRight, true);
  QVERIFY(modifier.held(false));
  QVERIFY(modifier.held(true));
  modifier.set(firstRight, false);
  QVERIFY(modifier.any());
  QVERIFY(modifier.held(!firstRight));
  QVERIFY(!modifier.held(firstRight));
  modifier.set(!firstRight, false);
  QVERIFY(!modifier.any());
}

void InputStateTests::modifier_repeatedPress_doesNotRequireExtraRelease()
{
  ModifierKeyState modifier;
  modifier.set(false, true);
  modifier.set(false, true);
  modifier.set(false, false);
  QVERIFY(!modifier.any());
}

void InputStateTests::buttons_releaseAll_emitsEverySyntheticRelease()
{
  SyntheticMouseButtons<6> buttons;
  buttons.set(0, true);
  buttons.set(1, true);
  buttons.set(2, true);
  buttons.set(5, true);
  buttons.set(1, false);
  std::vector<size_t> released;
  buttons.releaseAll([&](size_t button) { released.push_back(button); });
  QVERIFY(released == std::vector<size_t>({0, 2, 5}));
  QVERIFY(!buttons.any());
}

void InputStateTests::buttons_releaseAll_emptyLedger_emitsNothing()
{
  SyntheticMouseButtons<6> buttons;
  std::vector<size_t> released;
  buttons.releaseAll([&](size_t button) { released.push_back(button); });
  QVERIFY(released.empty());
}

void InputStateTests::buttons_releaseAll_isIdempotent()
{
  SyntheticMouseButtons<6> buttons;
  buttons.set(2, true);
  int releases = 0;
  const auto release = [&](size_t) { ++releases; };
  buttons.releaseAll(release);
  buttons.releaseAll(release);
  QCOMPARE(releases, 1);
}

void InputStateTests::buttons_invalidIndex_isIgnored()
{
  SyntheticMouseButtons<6> buttons;
  buttons.set(6, true);
  buttons.set(static_cast<size_t>(-1), true);
  QVERIFY(!buttons.any());
}

void InputStateTests::windowsAsyncState_usesHighBit_data()
{
  QTest::addColumn<int>("state");
  QTest::addColumn<bool>("down");
  QTest::newRow("up") << 0x0000 << false;
  QTest::newRow("pressed-since-last-query") << 0x0001 << false;
  QTest::newRow("bit-seven-is-not-down") << 0x0080 << false;
  QTest::newRow("down") << 0x8000 << true;
  QTest::newRow("down-and-pressed") << 0x8001 << true;
  QTest::newRow("all-bits") << 0xffff << true;
}

void InputStateTests::windowsAsyncState_usesHighBit()
{
  QFETCH(int, state);
  QFETCH(bool, down);
  QCOMPARE(isAsyncKeyDown(static_cast<int16_t>(state)), down);
}

void InputStateTests::windowsHook_localKeyUp_overridesPreEventAsyncState()
{
  WindowsPhysicalKeyState state;
  state.update(65, false, false, [](size_t key) -> int16_t { return key == 65 ? -32768 : 0; });
  QCOMPARE(state.keys()[65], uint8_t(0));
}

void InputStateTests::windowsHook_localDownAndRepeat_keepCurrentTransition()
{
  WindowsPhysicalKeyState state;
  state.update(65, true, false, [](size_t) -> int16_t { return 0; });
  QCOMPARE(state.keys()[65], uint8_t(0x80));
  state.update(65, true, false, [](size_t key) -> int16_t { return key == 65 ? -32768 : 0; });
  QCOMPARE(state.keys()[65], uint8_t(0x80));
}

void InputStateTests::windowsHook_suppressedHeldKey_survivesRefresh_data()
{
  QTest::addColumn<bool>("relay");
  QTest::newRow("while-relaying") << true;
  QTest::newRow("after-returning-local") << false;
}

void InputStateTests::windowsHook_suppressedHeldKey_survivesRefresh()
{
  QFETCH(bool, relay);
  WindowsPhysicalKeyState state;
  // The suppressed remote down is absent from Windows' async snapshot.
  state.update(65, true, true, [](size_t) -> int16_t { return 0; });
  state.update(66, true, relay, [](size_t key) -> int16_t { return key == 66 ? -32768 : 0; });
  QCOMPARE(state.keys()[65], uint8_t(0x80));
  QCOMPARE(state.keys()[66], uint8_t(0x80));
}

void InputStateTests::windowsHook_suppressedRelease_isNotResurrected()
{
  WindowsPhysicalKeyState state;
  const auto allUp = [](size_t) -> int16_t { return 0; };
  state.update(65, true, true, allUp);
  state.update(65, false, true, allUp);
  // An unrelated event can find stale OS downs, including the remote key
  // whose release the hook already observed and suppressed.
  state.update(66, true, false, [](size_t key) -> int16_t { return key == 65 || key == 66 ? -32768 : 0; });
  QCOMPARE(state.keys()[65], uint8_t(0));
  QCOMPARE(state.keys()[66], uint8_t(0x80));
}

void InputStateTests::windowsHook_modifierAliases_followResultingSides_data()
{
  QTest::addColumn<int>("generic");
  QTest::addColumn<int>("released");
  QTest::addColumn<int>("held");
  using State = WindowsPhysicalKeyState;
  QTest::newRow("shift-left-release") << int(State::Shift) << int(State::LeftShift) << int(State::RightShift);
  QTest::newRow("shift-right-release") << int(State::Shift) << int(State::RightShift) << int(State::LeftShift);
  QTest::newRow("control-left-release") << int(State::Control) << int(State::LeftControl) << int(State::RightControl);
  QTest::newRow("control-right-release") << int(State::Control) << int(State::RightControl) << int(State::LeftControl);
  QTest::newRow("alt-left-release") << int(State::Alt) << int(State::LeftAlt) << int(State::RightAlt);
  QTest::newRow("alt-right-release") << int(State::Alt) << int(State::RightAlt) << int(State::LeftAlt);
}

void InputStateTests::windowsHook_modifierAliases_followResultingSides()
{
  QFETCH(int, generic);
  QFETCH(int, released);
  QFETCH(int, held);
  WindowsPhysicalKeyState state;
  const auto allUp = [](size_t) -> int16_t { return 0; };
  state.update(released, true, false, allUp);
  QCOMPARE(state.keys()[generic], uint8_t(0x80));
  state.update(held, true, false, allUp);
  state.update(released, false, false, [&](size_t key) -> int16_t {
    return key == released || key == held ? -32768 : 0;
  });
  QCOMPARE(state.keys()[released], uint8_t(0));
  QCOMPARE(state.keys()[held], uint8_t(0x80));
  QCOMPARE(state.keys()[generic], uint8_t(0x80));
  state.update(held, false, false, [&](size_t key) -> int16_t { return key == held ? -32768 : 0; });
  QCOMPARE(state.keys()[generic], uint8_t(0));
}

void InputStateTests::windowsHook_reset_discardsOldState()
{
  WindowsPhysicalKeyState state;
  state.update(65, true, true, [](size_t) -> int16_t { return 0; });
  state.reset();
  QVERIFY(std::all_of(state.keys().begin(), state.keys().end(), [](uint8_t key) { return key == 0; }));
  state.update(66, true, false, [](size_t key) -> int16_t { return key == 65 || key == 66 ? -32768 : 0; });
  QCOMPARE(state.keys()[65], uint8_t(0x80));
}

void InputStateTests::windowsHook_localTransition_resumesAsyncRecovery()
{
  WindowsPhysicalKeyState state;
  const auto allUp = [](size_t) -> int16_t { return 0; };
  state.update(65, true, true, allUp);
  state.update(65, false, false, allUp);
  QCOMPARE(state.keys()[65], uint8_t(0));
  // Once a local transition resumes normal OS delivery, a later refresh may
  // recover an unobserved physical down for that key again.
  state.update(66, true, false, [](size_t key) -> int16_t { return key == 65 || key == 66 ? -32768 : 0; });
  QCOMPARE(state.keys()[65], uint8_t(0x80));
}

void InputStateTests::windowsHook_firstLocalDown_seedsPreHeldControl()
{
  using State = WindowsPhysicalKeyState;
  State state;
  // Control was physically down before installation. The first C down has
  // not reached the async state yet when the low-level callback runs.
  state.update(67, true, false, [](size_t key) -> int16_t { return key == State::LeftControl ? -32768 : 0; });
  QCOMPARE(state.keys()[State::LeftControl], uint8_t(0x80));
  QCOMPARE(state.keys()[State::Control], uint8_t(0x80));
  QCOMPARE(state.keys()[67], uint8_t(0x80));
}

void InputStateTests::windowsHook_firstRelayDown_seedsPreHeldModifiers()
{
  using State = WindowsPhysicalKeyState;
  State state;
  // The mouse can leave the host before any keyboard event reaches the hook.
  const auto preHeld = [](size_t key) -> int16_t {
    return key == State::LeftShift || key == State::RightControl ? -32768 : 0;
  };
  state.update(67, true, true, preHeld);
  QCOMPARE(state.keys()[State::LeftShift], uint8_t(0x80));
  QCOMPARE(state.keys()[State::RightControl], uint8_t(0x80));
  QCOMPARE(state.keys()[State::Shift], uint8_t(0x80));
  QCOMPARE(state.keys()[State::Control], uint8_t(0x80));
  QCOMPARE(state.keys()[67], uint8_t(0x80));
  // Hook reinstall/reset must allow a fresh initial snapshot as well.
  state.reset();
  state.update(67, true, true, preHeld);
  QCOMPARE(state.keys()[State::Shift], uint8_t(0x80));
  QCOMPARE(state.keys()[State::Control], uint8_t(0x80));
}

QTEST_GUILESS_MAIN(InputStateTests)
