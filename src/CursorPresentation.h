#pragma once

#include "Settings.h"
#include <optional>

namespace overworld {
// These boundaries are fake-only in CTest. No platform visibility API is linked there.
class CursorOverlay {
public:
    virtual ~CursorOverlay() = default;
    virtual bool present(int size, CursorStyle style) = 0;
    virtual void hide() = 0;
    virtual void reset() = 0;
    virtual const std::wstring& error() const = 0;
};

enum class NativeCursorState { Visible, Hidden, Unknown };
class CursorVisibility {
public:
    virtual ~CursorVisibility() = default;
    virtual bool initialize() = 0;
    virtual NativeCursorState observe() = 0;
    virtual bool show(bool visible) = 0;
    virtual bool shutdown() = 0;
    virtual const std::wstring& error() const = 0;
};

// UI-thread serialized: restore before discarding a published image. Not an OS visibility lease.
class CursorPresentation {
public:
    CursorPresentation(CursorOverlay& overlay, CursorVisibility& visibility) : overlay_(overlay), visibility_(visibility) {}
    void update(bool controller_active, const Settings& settings, bool explicit_app_desktop = false);
    bool pause(bool retry_restore = false);
    bool apply();
    bool enable(); // A deliberate Desktop-mode enable re-arms a suspended custom cursor.
    bool restore(); // Deliberate user recovery may show even when this process did not hide.
    bool shutdown();
    [[nodiscard]] bool owns_hide() const { return owns_hide_; }
    [[nodiscard]] bool recovery_pending() const { return restore_failed_; }
    [[nodiscard]] bool replacement_suspended() const { return suspended_; }
    [[nodiscard]] std::wstring error() const;

private:
    bool release(bool retry);
    void fail(std::wstring_view reason);
    CursorOverlay& overlay_;
    CursorVisibility& visibility_;
    bool owns_hide_ = false;
    bool restore_failed_ = false;
    bool suspended_ = false;
    std::wstring error_;
};

enum class CursorSurfaceObservation { Other, Start, Unknown };

struct CursorSurfaceRoot {
    std::wstring_view executable;
    std::wstring_view window_class;
    bool identity_valid = false;
    bool visible = false;
    bool iconic = false;
    std::optional<bool> cloaked;
};

class CursorImageQuerySchedule {
public:
    bool take(bool image_known, Tick now)
    {
        if (image_known || (attempted_at_ && now >= *attempted_at_ && now - *attempted_at_ < 1000)) { return false; }
        attempted_at_ = now;
        return true;
    }
    void reset() { attempted_at_.reset(); }
private:
    std::optional<Tick> attempted_at_;
};

[[nodiscard]] CursorSurfaceObservation classify_cursor_surface(const CursorSurfaceRoot& root);
[[nodiscard]] CursorSurfaceObservation combine_cursor_surfaces(CursorSurfaceObservation foreground,
    CursorSurfaceObservation hit);

// Presentation only. Uncertainty cannot reacquire hiding after entering Start.
class StartCursorFallback {
public:
    void update(CursorPresentation& cursor, CursorSurfaceObservation observation, bool controller_active,
        const Settings& settings, bool explicit_app_desktop = false);
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] std::wstring_view status() const;
private:
    bool active_ = false;
    bool uncertain_ = false;
};
} // namespace overworld
