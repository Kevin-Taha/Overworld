#include "KeyboardPlacement.h"
#include "KeyboardVisual.h"

#include <array>
#include <climits>
#include <iostream>
#include <stdexcept>

using namespace gamepad;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
bool equal(RECT a, RECT b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
bool overlaps(RECT a, RECT b)
{
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
void placement()
{
    for (const UINT dpi : {96u, 144u, 192u}) {
        for (const int height : {keyboard_visual_height, keyboard_prediction_height}) {
            const auto scale = [&](int value) { return MulDiv(value, dpi, 96); };
            for (const LONG origin : {0L, -5000L, 5000L}) {
                const RECT work{origin, -300, origin + scale(1920), -300 + scale(1040)};
                const RECT shell{origin + scale(650), -300 + scale(400), origin + scale(1270), work.bottom};
                const auto ordinary = keyboard_popup_bounds(work, dpi, height);
                const auto moved = keyboard_popup_bounds(work, dpi, height, {}, shell);
                check(ordinary && overlaps(*ordinary, shell), "fixture covers normal bottom-center keyboard");
                check(moved && !overlaps(*moved, shell), "relocated keyboard avoids shell at every DPI and negative monitor origin");
                check(moved->right - moved->left == scale(keyboard_visual_width)
                    && moved->bottom - moved->top == scale(height), "avoidance retains full renderer size and glyph scale");
                check(moved->left >= work.left && moved->right <= work.right && moved->top >= work.top && moved->bottom <= work.bottom,
                    "relocation stays inside target work area");
                const auto stable = keyboard_popup_bounds(work, dpi, height, *moved, shell);
                check(stable && equal(*stable, *moved), "clear current position never jumps on idle updates");
                const auto exited = keyboard_popup_bounds(work, dpi, height, *moved);
                check(exited && equal(*exited, *moved), "shell disappearance does not move an established popup");
            }
        }
    }
    const RECT work{0, 0, 1000, 800};
    const RECT bottom{0, 430, 1000, 800};
    const auto above = keyboard_popup_bounds(work, 96, keyboard_visual_height, {}, bottom);
    check(above && above->bottom <= 422, "full-width bottom shell selects space above with clearance");
    const auto expanded = keyboard_popup_bounds(work, 96, keyboard_prediction_height, *above, bottom);
    check(expanded && expanded->bottom <= 422 && expanded->bottom - expanded->top == keyboard_prediction_height,
        "suggestion growth searches again rather than growing underneath shell");
    const RECT left{0, 0, 500, 800};
    const auto right = keyboard_popup_bounds(work, 96, keyboard_visual_height, {}, left);
    check(right && right->left >= 508, "left shell chooses right-hand space");
    const RECT top{0, 0, 1000, 360};
    const auto below = keyboard_popup_bounds(work, 96, keyboard_visual_height, RECT{300, 0, 692, 350}, top);
    check(below && below->top >= 368, "top shell chooses space below");
    check(!keyboard_popup_bounds(work, 96, keyboard_visual_height, {}, work), "full-monitor shell explicitly has no placement");
    check(!keyboard_popup_bounds({0, 0, 300, 200}, 96, keyboard_visual_height, {}, RECT{0, 0, 100, 100}),
        "shell avoidance never silently shrinks keyboard into unusable controls");
    const auto original = keyboard_popup_bounds(work, 96, keyboard_visual_height);
    const auto offscreen = keyboard_popup_bounds(work, 96, keyboard_visual_height, {}, RECT{-2000, 0, -1500, 800});
    check(original && offscreen && equal(*original, *offscreen), "obstruction on another monitor leaves normal placement unchanged");
    check(!keyboard_popup_bounds(work, 96, keyboard_visual_height, {}, RECT{10, 10, 0, 0}), "malformed obstruction fails explicitly");
    check(!keyboard_popup_bounds({INT_MIN, 0, INT_MAX, 1000}, 96, keyboard_visual_height), "overflowing work geometry rejected");
    const RECT edge{INT_MAX - 1500, INT_MIN, INT_MAX, INT_MIN + 1000};
    const auto bounded = keyboard_popup_bounds(edge, 96, keyboard_visual_height, {},
        RECT{INT_MAX - 600, INT_MIN + 300, INT_MAX, INT_MIN + 1000});
    check(bounded && bounded->left >= edge.left && bounded->right <= edge.right, "edge-of-integer-space clearance uses wide arithmetic");

    // Compare candidate search with every possible integral placement on a small work area.
    const RECT compact{0, 0, 450, 400};
    for (const RECT obstacle : {RECT{0, 0, 20, 20}, RECT{200, 180, 250, 220}, RECT{0, 0, 450, 30},
        RECT{0, 350, 450, 400}, RECT{400, 0, 450, 400}, RECT{0, 0, 450, 400}}) {
        bool possible = false;
        const RECT padded{obstacle.left - 8, obstacle.top - 8, obstacle.right + 8, obstacle.bottom + 8};
        for (LONG y = 0; y <= 400 - keyboard_visual_height; ++y) {
            for (LONG x = 0; x <= 450 - keyboard_visual_width; ++x) {
                possible = possible || !overlaps({x, y, x + keyboard_visual_width, y + keyboard_visual_height}, padded);
            }
        }
        const auto result = keyboard_popup_bounds(compact, 96, keyboard_visual_height, {}, obstacle);
        check(result.has_value() == possible, "bounded candidate search finds a spot whenever one exists");
    }
}
void classification()
{
    check(keyboard_shell_image(L"StartMenuExperienceHost.exe") && keyboard_shell_image(L"SEARCHHOST.EXE")
        && keyboard_shell_image(L"SearchApp.exe"), "known shell images are case-insensitive");
    for (const auto name : {L"explorer.exe", L"editor.exe", L"SearchHost.exe.bak", L"OtherStartMenuExperienceHost.exe", L""}) {
        check(!keyboard_shell_image(name), "ordinary or lookalike processes are not shell placement exceptions");
    }
}
}
int main()
{
    try { classification(); placement(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS bounded shell avoidance, stable full-size placement, DPI/negative geometry and explicit no-space; no native windows.\n";
}
