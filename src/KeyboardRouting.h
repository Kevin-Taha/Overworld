#pragma once

#include "Desktop.h"
#include "KeyboardDelivery.h"

namespace gamepad {
[[nodiscard]] inline KeyboardEligibility observe_keyboard_eligibility(live::OutputBoundary& desktop,
    const KeyboardDelivery& keyboard, bool safe, bool native_enabled, bool desktop_paused,
    std::optional<unsigned> preferred)
{
    bool native = false;
    if (safe && native_enabled && !keyboard.recovery_required()) {
        native = keyboard.native_session();
        // Mouse-route observation mutates Desktop's shared context. Only a new
        // opener needs it; visible sessions validate their original text target.
        if (!keyboard.visible()) {
            const auto observation = desktop.observe(0);
            native = observation.available && observation.baseline_known && observation.routing_stable
                && !observation.physical_held && observation.mode == live::Mode::Pointer
                && live::keyboard_route_allowed(observation, true)
                && (desktop_paused || observation.block == live::BlockReason::Auto);
        }
    }
    return {safe && !desktop_paused && !native, preferred, native};
}
} // namespace gamepad
