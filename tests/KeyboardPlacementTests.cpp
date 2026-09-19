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
void regions_and_compact()
{
    const RECT work{0, 0, 2400, 1500};
    const RECT panel{492, 340, 1908, 1500};
    const auto region = keyboard_shell_obstruction(work, work, panel);
    check(region && equal(*region, panel), "fullscreen root with smaller native region uses actual region envelope");
    check(!keyboard_popup_bounds(work, 144, keyboard_visual_height, {}, *region), "full-size keyboard does not fit narrow shell side gaps");
    for (const int height : {keyboard_visual_height, keyboard_prediction_height}) {
        const auto compact = keyboard_popup_placement(work, 144, height, {}, *region);
        check(compact && compact->percent >= 80 && compact->percent < 100 && !overlaps(compact->bounds, panel),
            "bounded compact fallback makes narrow shell side gaps usable");
        const auto width = compact->bounds.right - compact->bounds.left;
        check(width * 100 >= MulDiv(keyboard_visual_width, 144, 96) * 80, "integer rounding never exceeds twenty percent reduction");
        check(!keyboard_popup_bounds(work, 144, height, {}, *region, compact->percent + 1),
            "chosen compact percentage is the largest that fits");
        const auto stable = keyboard_popup_placement(work, 144, height, compact->bounds, *region);
        check(stable && stable->percent == compact->percent && equal(stable->bounds, compact->bounds), "compact idle placement is stable");
        const auto normal = keyboard_popup_placement(work, 144, height, compact->bounds);
        check(normal && normal->percent == 100
            && normal->bounds.right - normal->bounds.left == MulDiv(keyboard_visual_width, 144, 96), "leaving shell restores native size");
    }
    const auto ordinary = keyboard_popup_placement(work, 144, keyboard_visual_height);
    const auto legacy = keyboard_popup_bounds(work, 144, keyboard_visual_height);
    check(ordinary && legacy && ordinary->percent == 100 && equal(ordinary->bounds, *legacy), "ordinary app placement is unchanged");
    check(!keyboard_popup_placement(work, 144, keyboard_visual_height, {}, RECT{470, 0, 1930, 1500}), "still fail if even twenty percent reduction cannot fit");
    check(!keyboard_popup_bounds(work, 144, keyboard_visual_height, {}, panel, 79), "explicit below-limit scale rejected");
    check(!keyboard_popup_bounds(work, 144, keyboard_visual_height, {}, {}, 90), "compact size is shell-only");
    check(!keyboard_popup_bounds(work, 96, keyboard_visual_height, {}, panel, 80), "never reduce below baseline pixel readability");
    const RECT window{-1800, -600, -600, 500};
    const RECT local{100, 20, 900, 800};
    const auto translated = keyboard_shell_obstruction(window, window, local);
    check(translated && equal(*translated, {-1700, -580, -900, 200}), "region coordinates translate from window origin on negative monitors");
    check(!keyboard_shell_obstruction(work, work, RECT{}), "empty region is unavailable, not free space");
    check(!keyboard_shell_obstruction(work, work, RECT{3000, 0, 4000, 100}), "nonintersecting malformed region rejected");
    const auto no_region = keyboard_shell_obstruction(panel, panel);
    check(no_region && equal(*no_region, panel), "windows without a region retain conservative DWM bounds");
    const auto edge = keyboard_shell_obstruction({INT_MAX - 1000, 0, INT_MAX, 800},
        {INT_MAX - 1000, 0, INT_MAX, 800}, RECT{100, 0, INT_MAX, 700});
    check(edge && edge->left == INT_MAX - 900 && edge->right == INT_MAX, "region translation clips without signed overflow");
}
void empty_focus_shell()
{
    const RECT work{0, 0, 2400, 1500};
    const auto focus_proxy = keyboard_shell_region(work, work, NULLREGION, {});
    check(focus_proxy.available && !focus_proxy.obstruction, "empty focused shell region is known non-drawing, not a query failure");
    const RECT start{492, 340, 1908, 1500};
    const auto peer = keyboard_shell_region(work, work, SIMPLEREGION, start);
    const auto merged = keyboard_shell_union(focus_proxy, peer, work);
    check(merged.available && merged.obstruction && equal(*merged.obstruction, start),
        "visible Start peer supplies obstruction while focused Search has no pixels");
    const auto placement = keyboard_popup_placement(work, 144, keyboard_visual_height, {}, merged.obstruction);
    check(placement && placement->percent >= 80 && !overlaps(placement->bounds, start), "focus proxy can display a compact keyboard beside visible shell");
    const RECT search{600, 340, 1800, 1500};
    const auto search_visible = keyboard_shell_region(search, search, SIMPLEREGION, RECT{0, 0, 1200, 1160});
    const auto handoff = keyboard_shell_union(search_visible, focus_proxy, work);
    check(handoff.available && handoff.obstruction && equal(*handoff.obstruction, search),
        "appearance of Search replaces empty peer without altering input identity");
    const auto together = keyboard_shell_union(peer, search_visible, work);
    check(together.obstruction && equal(*together.obstruction, start), "simultaneous shell surfaces are avoided conservatively");
    check(!keyboard_shell_union({false, {}}, peer, work).available, "real metadata failure is not confused with empty region");
    check(!keyboard_shell_region({}, work, NULLREGION, {}).available, "empty region cannot excuse invalid frame metadata");
    check(!keyboard_shell_region(work, work, SIMPLEREGION, {}).available, "malformed nonempty region remains an error");
    check(!keyboard_shell_region(work, work, 99, {}).available, "unknown region kind remains an error");
    const auto other_monitor = keyboard_shell_union(focus_proxy, {true, RECT{-1800, 0, -1200, 900}}, work);
    check(other_monitor.available && !other_monitor.obstruction, "other-monitor shell peer does not consume target monitor space");
}
}
int main()
{
    try { classification(); placement(); regions_and_compact(); empty_focus_shell(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS bounded shell avoidance, stable full-size placement, DPI/negative geometry and explicit no-space; no native windows.\n";
}
