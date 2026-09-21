#include "KeyboardAppearance.h"

namespace overworld {
namespace {
std::uint32_t rgb(COLORREF value)
{
    return (static_cast<std::uint32_t>(GetRValue(value)) << 16)
        | (static_cast<std::uint32_t>(GetGValue(value)) << 8) | GetBValue(value);
}
}

HRESULT read_keyboard_theme(KeyboardTheme& theme)
{
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) {
        const auto error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    theme = {};
    theme.high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    if (theme.high_contrast) {
        theme.window = rgb(GetSysColor(COLOR_WINDOW));
        theme.window_text = rgb(GetSysColor(COLOR_WINDOWTEXT));
        theme.highlight = rgb(GetSysColor(COLOR_HIGHLIGHT));
        theme.highlight_text = rgb(GetSysColor(COLOR_HIGHLIGHTTEXT));
        return S_OK;
    }
    DWORD light = 1, size = sizeof(light);
    const auto result = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    // Missing is the Windows light-theme default, not a failed read.
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND) {
        return HRESULT_FROM_WIN32(result);
    }
    theme.dark = result == ERROR_SUCCESS && light == 0;
    return S_OK;
}
} // namespace overworld
