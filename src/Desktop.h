#pragma once

#include "Live.h"
#include "TextDelivery.h"
#include "NavigationFilter.h"
#include "PredictionTarget.h"

#include <Windows.h>
#include <string>

namespace gamepad::live {

[[nodiscard]] inline bool keyboard_route_allowed(const Observation& observation, bool native_session)
{
    return observation.allowed || (native_session && observation.block == BlockReason::Auto
        && heuristic_auto(observation.automatic));
}

constexpr UINT context_message = WM_APP + 2;
constexpr ULONG_PTR output_tag = 0x47445031;

[[nodiscard]] inline WORD text_virtual_key(TextKey key)
{
    switch (key.kind) {
    case TextKeyKind::Backspace: return VK_BACK;
    case TextKeyKind::Enter: return VK_RETURN;
    case TextKeyKind::Unicode: return VK_PACKET;
    }
    return 0;
}

[[nodiscard]] inline bool text_key_conflict(unsigned key, bool asynchronous, bool physical,
    std::optional<TextKey> owned)
{
    return owned && key == text_virtual_key(*owned) ? physical : asynchronous;
}

[[nodiscard]] inline DWORD routing_flags(DWORD flags) { return flags & ~GUI_CARETBLINKING; }

[[nodiscard]] inline bool raw_keyboard_activity(bool hardware, unsigned key, ULONG_PTR extra)
{
    return hardware || (extra != output_tag && physical_async_key(key));
}

struct FocusEvent {
    TextWindow window;
    LONG object = 0;
    LONG child = 0;
    bool operator==(const FocusEvent&) const = default;
};

class FocusTracker {
public:
    bool observe(TextWindow foreground, TextWindow focused)
    {
        if (foreground == foreground_ && focused == focused_) { return false; }
        foreground_ = foreground;
        focused_ = focused;
        semantic_.reset();
        ++revision_;
        return true;
    }
    bool notify(FocusEvent event, bool relevant)
    {
        if (!relevant || (semantic_ && *semantic_ == event)) { return false; }
        semantic_ = event;
        ++revision_;
        return true;
    }
    void invalidate()
    {
        semantic_.reset();
        ++revision_;
    }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
private:
    TextWindow foreground_, focused_;
    std::optional<FocusEvent> semantic_;
    std::uint64_t revision_ = 0;
};

[[nodiscard]] inline INPUT keyboard_input(TextEvent event)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    if (event.key.kind == TextKeyKind::Unicode) {
        input.ki.wScan = event.key.unit;
        input.ki.dwFlags = KEYEVENTF_UNICODE;
    } else { input.ki.wVk = text_virtual_key(event.key); }
    if (event.up) { input.ki.dwFlags |= KEYEVENTF_KEYUP; }
    input.ki.dwExtraInfo = output_tag;
    return input;
}

// UI-thread-only native boundary. No input hooks, focus changes, or output worker.
class Desktop final : public OutputBoundary, public TextBoundary {
public:
    ~Desktop() override;
    bool initialize(HWND host, bool emergency_ready);
    void shutdown();
    bool raw_input(HRAWINPUT input);
    void session_changed(WPARAM event);
    void invalidate();
    void set_automatic_game_switching(bool enabled);
    [[nodiscard]] bool available() const { return ready_; }
    [[nodiscard]] const std::wstring& infrastructure() const { return infrastructure_; }
    [[nodiscard]] const std::wstring& attention_error() const { return attention_error_; }
    [[nodiscard]] std::wstring diagnostic_summary() const;
    [[nodiscard]] NavigationForeground observed_foreground() const {
        if (!last_routing_) { return {}; }
        const auto& window = last_routing_->windows[0];
        return {reinterpret_cast<std::uintptr_t>(window.window), window.process, window.thread};
    }

    Observation observe(unsigned owned, const Output* intent = nullptr) override;
    Tick now() const override;
    unsigned send(const Output& output, Rect desktop) override;
    TextObservation observe_text(std::optional<TextKey> owned) override;
    Tick text_now() const override { return now(); }
    unsigned send_text(TextEvent event) override;

private:
    static void CALLBACK event_callback(HWINEVENTHOOK, DWORD event, HWND window,
                                        LONG object, LONG child, DWORD thread, DWORD time);
    bool normal_desktop() const;
    bool allowed_process(DWORD process, std::wstring* basename = nullptr);
    unsigned physical_state(unsigned owned, std::optional<TextKey> text_owned = {});

    struct WindowState {
        HWND window = nullptr;
        DWORD process = 0;
        DWORD thread = 0;
        bool visible = false;
        bool enabled = false;
        bool operator==(const WindowState&) const = default;
    };
    struct Routing {
        // Foreground, active, focus, capture, menu owner, point window, point root.
        std::array<WindowState, 7> windows{};
        DWORD flags = 0;
        Point cursor{};
        Rect desktop{};
        AppIdentity app;
        bool operator==(const Routing& other) const
        {
            // Dragged window rectangles and asynchronous cursor motion are not recipient identity.
            // Non-motion sends separately revalidate the cursor immediately before insertion.
            return windows == other.windows && flags == other.flags && desktop == other.desktop && app == other.app;
        }
    };
    bool read_routing(const Output* intent, Routing& routing, bool text = false);
    Observation observe_route(unsigned owned, const Output* intent, std::optional<TextKey> text_owned, Routing* text_route);
    std::wstring_view attention_pause_reason(HWND foreground);
    AppActivation update_activation(AppIdentity identity);
    enum class Query { None, Session, ProcessOpen, Token, Image, ForegroundThread, GuiThread, WindowIdentity, Cursor };
    struct Diagnostic {
        BlockReason block = BlockReason::Infrastructure;
        Query query = Query::None;
        DWORD error = 0;
        int role = -1;
        HWND window = nullptr;
        DWORD process = 0, thread = 0, session = 0, integrity = 0;
        bool token_known = false, ui_access = false;
        InputCategory physical = InputCategory::None;
        bool physical_raw = false, physical_async = false, settling = false;
        DWORD shell_process = 0;
        bool shell_candidate = false;
        AppActivation app;
    } diagnostic_;

    static Desktop* instance_;
    HWND host_ = nullptr;
    DWORD session_ = 0;
    DWORD integrity_ = 0;
    HWINEVENTHOOK foreground_hook_ = nullptr;
    HWINEVENTHOOK focus_hook_ = nullptr;
    bool raw_registered_ = false;
    bool session_registered_ = false;
    bool ready_ = false;
    bool unlocked_ = true;
    bool unsafe_ = false;
    bool physical_escape_ = false;
    std::array<bool, 256> physical_keys_{};
    unsigned physical_mouse_ = 0;
    std::uint64_t generation_ = 1;
    std::uint64_t activity_ = 0;
    FocusTracker focus_;
    std::uint64_t routing_revision_ = 0;
    std::optional<Routing> last_routing_;
    HWND attention_foreground_ = nullptr;
    std::optional<Tick> attention_checked_;
    bool attention_valid_ = false;
    NotificationState attention_state_ = NotificationState::NotPresent;
    bool automatic_game_switching_ = true;
    bool fullscreen_capture_ = false;
    DWORD attention_shell_process_ = 0;
    DWORD attention_process_ = 0;
    GameHandoff game_handoff_;
    std::uint64_t activation_generation_ = 0;
    AutoReason auto_reason_ = AutoReason::None;
    ReleasedKeyState released_keys_;
    bool input_settling_ = false;
    std::wstring attention_error_;
    std::wstring infrastructure_ = L"Live observers not initialized";
};

} // namespace gamepad::live
