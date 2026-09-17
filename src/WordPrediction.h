#pragma once

#include "Keyboard.h"

namespace gamepad {
inline constexpr std::size_t prediction_prefix_limit = 16;
inline constexpr std::size_t prediction_choice_limit = 3;

struct PredictionView {
    std::array<std::wstring, prediction_choice_limit> words{};
    std::size_t count = 0;
    std::size_t selected = 0;
    std::uint64_t generation = 0;
    bool operator==(const PredictionView&) const = default;
};

// Transport-completed edits are hints, not a copy of the application's text.
// Nothing is read, learned or persisted. An explicit separator establishes a word boundary.
class WordPrediction {
public:
    void clear();
    void completed(KeyboardEdit edit);
    void select(bool next);
    [[nodiscard]] std::optional<std::u32string> accept(std::uint64_t generation);
    [[nodiscard]] const PredictionView& view() const { return view_; }
    [[nodiscard]] std::wstring_view prefix() const { return prefix_; }

private:
    void refresh();
    std::wstring prefix_;
    PredictionView view_;
    bool known_boundary_ = false;
};
} // namespace gamepad
