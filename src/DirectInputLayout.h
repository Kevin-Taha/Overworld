#pragma once

#include "ControllerMapping.h"

#include <Windows.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace overworld {
struct DirectInputAxis {
    DWORD type = 0, offset = 0;
};
struct DirectInputLayout {
    std::array<DirectInputAxis, controller_axes> axes{};
    ControllerLayout controls;
    unsigned sliders = 0;

    [[nodiscard]] bool add(const DIDEVICEOBJECTINSTANCEW& object)
    {
        if (object.dwType & DIDFT_AXIS) {
            if (!(object.dwType & DIDFT_ABSAXIS) || controls.axes == axes.size()) { return false; }
            const std::array<const GUID*, 6> types{&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
            constexpr std::array<DWORD, 6> offsets{offsetof(DIJOYSTATE2, lX), offsetof(DIJOYSTATE2, lY),
                offsetof(DIJOYSTATE2, lZ), offsetof(DIJOYSTATE2, lRx), offsetof(DIJOYSTATE2, lRy), offsetof(DIJOYSTATE2, lRz)};
            DWORD offset = sizeof(DIJOYSTATE2);
            for (unsigned i = 0; i < types.size(); ++i) { if (object.guidType == *types[i]) { offset = offsets[i]; break; } }
            if (object.guidType == GUID_Slider && sliders < 2) {
                offset = static_cast<DWORD>(offsetof(DIJOYSTATE2, rglSlider) + sliders++ * sizeof(LONG));
            }
            if (offset == sizeof(DIJOYSTATE2)) { return false; }
            axes[controls.axes++] = {object.dwType, offset};
        } else if (object.dwType & DIDFT_BUTTON) {
            if (controls.buttons == controller_buttons) { return false; }
            ++controls.buttons;
        } else if (object.dwType & DIDFT_POV) {
            if (controls.hats == controller_hats) { return false; }
            ++controls.hats;
        }
        return true;
    }

    void order()
    {
        // SDL retains repeated axis types as logical aliases of the same native field.
        std::sort(axes.begin(), axes.begin() + controls.axes, [](const auto& left, const auto& right) {
            return left.offset < right.offset;
        });
    }
};

[[nodiscard]] inline bool directinput_report(const DirectInputLayout& layout, const DIJOYSTATE2& state, RawController& raw)
{
    RawController parsed;
    for (unsigned i = 0; i < layout.controls.axes; ++i) {
        LONG value = 0;
        std::memcpy(&value, reinterpret_cast<const unsigned char*>(&state) + layout.axes[i].offset, sizeof(value));
        if (value < -32768 || value > 32767) { return false; }
        parsed.axes[i] = static_cast<std::int16_t>(value);
    }
    for (unsigned i = 0; i < layout.controls.buttons; ++i) { parsed.buttons[i] = (state.rgbButtons[i] & 0x80) != 0; }
    for (unsigned i = 0; i < layout.controls.hats; ++i) {
        const auto angle = state.rgdwPOV[i];
        if ((angle & 0xffff) == 0xffff) { continue; }
        if (angle >= 36000) { return false; }
        constexpr std::array<std::uint8_t, 8> hats{1, 3, 2, 6, 4, 12, 8, 9};
        parsed.hats[i] = hats[((angle + 2250) % 36000) / 4500];
    }
    raw = parsed;
    return true;
}
} // namespace overworld
