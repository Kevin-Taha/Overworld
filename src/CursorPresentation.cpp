#include "CursorPresentation.h"

namespace gamepad {
void CursorPresentation::fail(std::wstring_view reason)
{
    suspended_ = true;
    error_ = std::wstring{reason};
    if (!visibility_.error().empty()) { error_ += L" " + visibility_.error(); }
}

bool CursorPresentation::release(bool retry)
{
    if (!owns_hide_) { return !restore_failed_; }
    if (restore_failed_ && !retry) { return false; } // No 16-ms retry loop against global visibility.
    if (!visibility_.show(true)) {
        restore_failed_ = true;
        fail(L"Windows pointer restore FAILED. Last overlay retained; Desktop mode paused. Use Restore Windows pointer.");
        return false;
    }
    owns_hide_ = false;
    if (restore_failed_) { error_.clear(); }
    restore_failed_ = false;
    return true;
}

bool CursorPresentation::pause(bool retry_restore)
{
    if (!release(retry_restore)) { return false; }
    overlay_.hide();
    return true;
}

void CursorPresentation::update(bool controller_active, const Settings& settings, bool explicit_app_desktop)
{
    if (restore_failed_) { return; }
    if (!controller_active || suspended_) { pause(); return; }
    // present() must retain the last published image on failure; we may still own native hiding.
    if (!overlay_.present(settings.ring_diameter, settings.cursor_style)) {
        pause();
        return;
    }
    if (owns_hide_) { return; }
    if (!visibility_.initialize()) {
        fail(L"Cursor replacement unavailable; native hiding was not requested.");
        overlay_.hide();
        return;
    }
    const auto state = visibility_.observe();
    // A deliberate app-scoped escape may show only our overlay over an externally
    // hidden cursor. No native visibility mutation or hide ownership is acquired.
    if (state == NativeCursorState::Hidden && explicit_app_desktop) { return; }
    if (state != NativeCursorState::Visible) {
        fail(state == NativeCursorState::Hidden
            ? L"Native cursor was already hidden/suppressed; replacement will not take ownership or show it."
            : L"Native cursor visibility is unknown; replacement did not hide it.");
        overlay_.hide();
        return;
    }
    if (!visibility_.show(false)) {
        fail(L"Cursor replacement hide FAILED; no hide ownership recorded. Use Restore Windows pointer if the cursor is missing.");
        overlay_.hide();
        return;
    }
    owns_hide_ = true;
}

bool CursorPresentation::apply()
{
    if (!pause()) { return false; }
    overlay_.reset();
    return true;
}

bool CursorPresentation::enable()
{
    if (!apply()) { return false; }
    suspended_ = false;
    error_.clear();
    return true;
}

bool CursorPresentation::restore()
{
    // Only a new explicit Desktop-mode enable may undo this user recovery.
    suspended_ = true;
    if (!visibility_.initialize() || !visibility_.show(true)) {
        restore_failed_ = true;
        fail(L"Explicit Windows pointer restore FAILED. Retry Restore Windows pointer; replacement remains suspended.");
        return false;
    }
    owns_hide_ = false;
    restore_failed_ = false;
    error_.clear();
    overlay_.hide();
    return true;
}

bool CursorPresentation::shutdown()
{
    if (!pause(true)) { return false; }
    if (!visibility_.shutdown()) {
        fail(L"Cursor runtime cleanup failed (native hiding is no longer owned).");
        return false;
    }
    return true;
}

std::wstring CursorPresentation::error() const
{
    if (overlay_.error().empty()) { return error_; }
    return error_.empty() ? overlay_.error() : error_ + L"\r\n" + overlay_.error();
}

CursorSurfaceObservation classify_cursor_surface(const CursorSurfaceRoot& root)
{
    if (!root.identity_valid || root.window_class.empty()) { return CursorSurfaceObservation::Unknown; }
    if (!root.visible || root.iconic || root.window_class != L"Windows.UI.Core.CoreWindow") {
        return CursorSurfaceObservation::Other;
    }
    if (!root.cloaked) { return CursorSurfaceObservation::Unknown; }
    if (*root.cloaked) { return CursorSurfaceObservation::Other; }
    if (root.executable.empty()) { return CursorSurfaceObservation::Unknown; }
    constexpr std::wstring_view expected = L"startmenuexperiencehost.exe";
    if (root.executable.size() != expected.size()) { return CursorSurfaceObservation::Other; }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto value = root.executable[i];
        const auto lower = value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
        if (lower != expected[i]) { return CursorSurfaceObservation::Other; }
    }
    return CursorSurfaceObservation::Start;
}

CursorSurfaceObservation combine_cursor_surfaces(CursorSurfaceObservation foreground, CursorSurfaceObservation hit)
{
    if (foreground == CursorSurfaceObservation::Start || hit == CursorSurfaceObservation::Start) {
        return CursorSurfaceObservation::Start;
    }
    return foreground == CursorSurfaceObservation::Other && hit == CursorSurfaceObservation::Other
        ? CursorSurfaceObservation::Other : CursorSurfaceObservation::Unknown;
}

void StartCursorFallback::update(CursorPresentation& cursor, CursorSurfaceObservation observation,
    bool controller_active, const Settings& settings, bool explicit_app_desktop)
{
    uncertain_ = observation == CursorSurfaceObservation::Unknown;
    if (!uncertain_) { active_ = observation == CursorSurfaceObservation::Start; }
    if (active_) { cursor.pause(); }
    else { cursor.update(controller_active, settings, explicit_app_desktop); }
}

std::wstring_view StartCursorFallback::status() const
{
    if (active_) {
        return uncertain_ ? L"Start cursor fallback retained: surface observation is inconclusive."
            : L"Start surface: native-cursor fallback requested; only owned hiding is restored.";
    }
    return uncertain_ ? L"Cursor surface observation is inconclusive; presentation policy unchanged." : L"";
}
} // namespace gamepad
