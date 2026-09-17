#pragma once

#include "TextDelivery.h"

namespace gamepad {
// Negative evidence only: Unknown never means verified safe/plain text.
// No target messages, accessibility requests, text, length or caret reads.
PredictionPrivacy prediction_target_privacy(TextWindow target);
} // namespace gamepad
