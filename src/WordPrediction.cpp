#include "WordPrediction.h"

#include <algorithm>

namespace overworld {
namespace {
constexpr std::string_view words[]{
#include "../third_party/PredictionVocabulary.inc"
};
constexpr bool upper(wchar_t c) { return c >= L'A' && c <= L'Z'; }
constexpr wchar_t lower(wchar_t c) { return upper(c) ? static_cast<wchar_t>(c + (L'a' - L'A')) : c; }
static_assert([] {
    for (auto word : words) {
        if (word.empty() || word.size() > prediction_word_limit) { return false; }
        for (auto c : word) { if (c < 'a' || c > 'z') { return false; } }
    }
    return true;
}());
}

std::span<const std::string_view> prediction_vocabulary() { return words; }

void WordPrediction::clear(bool collect)
{
    prefix_.clear();
    known_boundary_ = collect;
    const auto generation = view_.generation + 1;
    view_ = {};
    view_.generation = generation;
}

void WordPrediction::completed(KeyboardEdit edit)
{
    if (edit.kind == KeyboardEditKind::Enter
        || (edit.kind == KeyboardEditKind::Character && edit.character == U' ')) {
        clear();
        known_boundary_ = true;
    } else if (edit.kind == KeyboardEditKind::Backspace) {
        if (prefix_.empty()) { clear(); }
        else { prefix_.pop_back(); refresh(); }
    } else if (edit.kind == KeyboardEditKind::Character
        && ((edit.character >= U'a' && edit.character <= U'z') || (edit.character >= U'A' && edit.character <= U'Z'))) {
        if (!known_boundary_) { return; }
        if (prefix_.size() == prediction_prefix_limit) { clear(); return; }
        prefix_.push_back(static_cast<wchar_t>(edit.character));
        refresh();
    } else { clear(); }
}

void WordPrediction::refresh()
{
    const auto generation = view_.generation + 1;
    view_ = {};
    view_.generation = generation;
    if (prefix_.size() < 2) { return; }
    const bool capitals = std::all_of(prefix_.begin(), prefix_.end(), upper);
    for (std::size_t i = 1; i < prefix_.size(); ++i) {
        if (!capitals && upper(prefix_[i])) { return; }
    }
    std::string folded;
    for (auto c : prefix_) { folded.push_back(static_cast<char>(lower(c))); }
    for (auto word : words) {
        if (word.size() <= folded.size() || !word.starts_with(folded)) { continue; }
        auto& choice = view_.words[view_.count++];
        choice.assign(word.begin(), word.end());
        if (capitals) {
            for (auto& c : choice) { c = static_cast<wchar_t>(c - (L'a' - L'A')); }
        } else if (upper(prefix_.front())) {
            choice.front() = prefix_.front();
        }
        if (view_.count == prediction_choice_limit) { break; }
    }
}

void WordPrediction::select(bool next)
{
    if (!view_.count) { return; }
    view_.selected = (view_.selected + (next ? 1 : view_.count - 1)) % view_.count;
    ++view_.generation;
}

std::optional<std::u32string> WordPrediction::accept(std::uint64_t generation)
{
    if (generation != view_.generation || !view_.count || view_.selected >= view_.count) { return {}; }
    const auto& word = view_.words[view_.selected];
    if (prefix_.empty() || !word.starts_with(prefix_) || word.size() <= prefix_.size()) { return {}; }
    std::u32string suffix;
    for (auto c : std::wstring_view(word).substr(prefix_.size())) { suffix.push_back(static_cast<char32_t>(c)); }
    clear(); // One-shot; do not infer the completed word or replay acceptance.
    return suffix;
}
} // namespace overworld
