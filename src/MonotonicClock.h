#pragma once

#include "Core.h"
#include <chrono>

namespace overworld {
[[nodiscard]] inline Tick monotonic_now()
{
    return static_cast<Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace overworld
