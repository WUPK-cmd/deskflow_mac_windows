/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace deskflow::platform {

// Pure state used by native input adapters; no OS input is posted here.
class WheelAccumulator
{
public:
  int32_t add(int32_t delta, double scale = 1.0)
  {
    m_remainder += static_cast<long double>(delta) * scale;
    // Account for decimal settings such as 0.3 landing just below a whole
    // line after floating-point arithmetic. The tolerance is a trillionth
    // of one line and the emitted part is always removed from the remainder.
    const auto lines = m_remainder / 120.0L;
    const auto whole = static_cast<int32_t>(std::trunc(lines + std::copysign(1e-12L, lines)));
    m_remainder -= static_cast<long double>(whole) * 120.0L;
    return whole;
  }

  void reset()
  {
    m_remainder = 0;
  }

private:
  long double m_remainder = 0;
};

class ModifierKeyState
{
public:
  void set(bool right, bool pressed)
  {
    (right ? m_right : m_left) = pressed;
  }

  bool any() const
  {
    return m_left || m_right;
  }
  bool held(bool right) const
  {
    return right ? m_right : m_left;
  }

private:
  bool m_left = false;
  bool m_right = false;
};

template <size_t ButtonCount> class SyntheticMouseButtons
{
public:
  void set(size_t button, bool pressed)
  {
    if (button < ButtonCount) {
      m_pressed.set(button, pressed);
    }
  }

  template <typename Release> void releaseAll(Release release)
  {
    const auto pressed = m_pressed;
    m_pressed.reset();
    for (size_t button = 0; button < ButtonCount; ++button) {
      if (pressed.test(button)) {
        release(button);
      }
    }
  }

  bool any() const
  {
    return m_pressed.any();
  }

private:
  std::bitset<ButtonCount> m_pressed;
};

inline bool isAsyncKeyDown(int16_t state)
{
  return (static_cast<uint16_t>(state) & 0x8000u) != 0;
}

class WindowsPhysicalKeyState
{
public:
  static constexpr size_t KeyCount = 256;
  using Keys = std::array<uint8_t, KeyCount>;
  enum VirtualKey : size_t
  {
    Shift = 0x10,
    Control = 0x11,
    Alt = 0x12,
    LeftShift = 0xa0,
    RightShift = 0xa1,
    LeftControl = 0xa2,
    RightControl = 0xa3,
    LeftAlt = 0xa4,
    RightAlt = 0xa5
  };

  template <typename AsyncState> void update(size_t key, bool pressed, bool relay, AsyncState asyncState)
  {
    // Seed pre-existing physical holds before observing the first transition,
    // including when the first event arrives after the mouse has left the host.
    // Subsequent relayed events can be suppressed, so their OS state is not
    // authoritative, even after returning to the local host.
    if (!m_initialized || (!relay && isAsyncKeyDown(asyncState(key)))) {
      for (size_t i = 0; i < KeyCount; ++i) {
        if (!m_relayObserved.test(i)) {
          m_keys[i] = isAsyncKeyDown(asyncState(i)) ? 0x80 : 0;
        }
      }
    }

    // WH_KEYBOARD_LL runs before the current async state changes. Apply its
    // observed transition last, then derive aliases from the resulting sides.
    m_keys[key] = pressed ? 0x80 : 0;
    m_relayObserved.set(key, relay);
    m_keys[Shift] = m_keys[LeftShift] | m_keys[RightShift];
    m_keys[Control] = m_keys[LeftControl] | m_keys[RightControl];
    m_keys[Alt] = m_keys[LeftAlt] | m_keys[RightAlt];
    m_initialized = true;
  }

  const Keys &keys() const
  {
    return m_keys;
  }

  void reset()
  {
    m_keys.fill(0);
    m_relayObserved.reset();
    m_initialized = false;
  }

private:
  Keys m_keys{};
  std::bitset<KeyCount> m_relayObserved;
  bool m_initialized = false;
};

} // namespace deskflow::platform
