#include "WordPrediction.h"

#include <algorithm>

namespace gamepad {
namespace {
// Small, authored generic vocabulary. Order is stable, not learned or personalized.
constexpr std::wstring_view words[]{
    L"about", L"again", L"also", L"another", L"answer", L"around", L"back", L"because",
    L"before", L"better", L"between", L"both", L"build", L"call", L"came", L"change",
    L"check", L"close", L"come", L"could", L"done", L"down", L"each", L"early",
    L"easy", L"enough", L"every", L"example", L"find", L"first", L"follow", L"from",
    L"give", L"good", L"great", L"group", L"have", L"hello", L"help", L"here",
    L"home", L"hope", L"how", L"into", L"just", L"keep", L"know", L"later",
    L"learn", L"left", L"like", L"little", L"look", L"make", L"many", L"maybe",
    L"message", L"more", L"morning", L"most", L"much", L"name", L"need", L"never",
    L"next", L"nice", L"night", L"number", L"only", L"open", L"other", L"over",
    L"people", L"place", L"please", L"point", L"question", L"quite", L"read", L"ready",
    L"really", L"right", L"same", L"second", L"send", L"should", L"small", L"some",
    L"something", L"soon", L"start", L"still", L"stop", L"sure", L"take", L"tell",
    L"test", L"than", L"thank", L"thanks", L"that", L"their", L"them", L"then",
    L"there", L"these", L"they", L"thing", L"think", L"this", L"those", L"time",
    L"today", L"together", L"tomorrow", L"under", L"until", L"update", L"use", L"very",
    L"wait", L"want", L"way", L"week", L"well", L"were", L"what", L"when",
    L"where", L"which", L"while", L"will", L"with", L"work", L"world", L"would",
    L"write", L"year", L"yes", L"your"
};
constexpr bool upper(wchar_t c) { return c >= L'A' && c <= L'Z'; }
constexpr wchar_t lower(wchar_t c) { return upper(c) ? static_cast<wchar_t>(c + (L'a' - L'A')) : c; }
static_assert([] {
    for (auto word : words) {
        if (word.empty() || word.size() > prediction_prefix_limit) { return false; }
        for (auto c : word) { if (c < L'a' || c > L'z') { return false; } }
    }
    return true;
}());
}

void WordPrediction::clear()
{
    prefix_.clear();
    known_boundary_ = false;
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
    std::wstring folded = prefix_;
    std::transform(folded.begin(), folded.end(), folded.begin(), lower);
    for (auto word : words) {
        if (word.size() <= folded.size() || !word.starts_with(folded)) { continue; }
        auto& choice = view_.words[view_.count++];
        choice = word;
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
} // namespace gamepad
