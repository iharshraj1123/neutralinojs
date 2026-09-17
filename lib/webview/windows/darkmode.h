#include <dwmapi.h>  // DwmSetWindowAttribute()

// Returns Windows' build number.
// ntdll.dll -> RtlGetNtVersionNumbers
DWORD GetBuildNumber() {
    HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        typedef void(__stdcall*fnRtlGetNtVersionNumbers)(DWORD*, DWORD*, DWORD*);
        fnRtlGetNtVersionNumbers proc = (fnRtlGetNtVersionNumbers)GetProcAddress(hNtdll, "RtlGetNtVersionNumbers");  
        if (proc != nullptr) {
            DWORD dwMajor, dwMinor, dwBuildNumber;
            proc(&dwMajor, &dwMinor, &dwBuildNumber);
            dwBuildNumber &= ~0xF0000000;
            return dwBuildNumber;
        }
    }
    return 0;
}

// Build numbers 22000 and above are Windows 11.
bool IsWindows10() {
    return GetBuildNumber() < 22000;
}

// Returns whether High Contrast theme is enabled.
// user32.dll -> SystemParametersInfoW
bool IsHighContrast() {
    HMODULE hUser32 = ::GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI * fnSystemParametersInfo)(UINT, UINT, PVOID, UINT);
        fnSystemParametersInfo proc = (fnSystemParametersInfo)GetProcAddress(hUser32, "SystemParametersInfoW");
        if (proc) {
            HIGHCONTRASTW highContrast = { sizeof(highContrast) };
            if (proc(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, FALSE))
                return highContrast.dwFlags & HCF_HIGHCONTRASTON;
        }
    }
	return false;
}

// Returns whether system-wide dark mode is enabled.
// uxtheme.dll -> ShouldSystemUseDarkMode
bool IsDarkModePreferred() {
    HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxtheme) {
        typedef BOOLEAN (WINAPI * fnShouldAppsUseDarkMode)(); // ordinal 132
        fnShouldAppsUseDarkMode proc = (fnShouldAppsUseDarkMode)(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(132)));
        if (proc != nullptr) {
            return proc();
        }
    }
    return false;
}

// Sets flags to enable dark title bar in Windows 10 and 11.
// dwmapi.lib -> DwmSetWindowAttribute
HRESULT TrySetWindowTheme(HWND hWnd, bool dark) {
    const BOOL isDarkMode = dark;
    
    // set DWMWA_USE_IMMERSIVE_DARK_MODE
    // This flag works since Win10 20H1 but is not documented until Windows 11
    HRESULT result = DwmSetWindowAttribute(hWnd, 20, &isDarkMode, sizeof(isDarkMode));

    if (FAILED(result)) {
        // this would be the call before Windows build 18362
        result = DwmSetWindowAttribute(hWnd, 19, &isDarkMode, sizeof(isDarkMode));
    }

    if (FAILED(result))
        return result;

    // Toggle the nonclient area active state to force a redraw (Win10 workaround)
    if (IsWindows10()) {
        HWND activeWindow = GetActiveWindow();
        SendMessage(hWnd, WM_NCACTIVATE, hWnd != activeWindow, 0);
        SendMessage(hWnd, WM_NCACTIVATE, hWnd == activeWindow, 0);
    }

    return S_OK;
}

// Sets dark mode of title bar according to system theme.
HRESULT TrySetWindowTheme(HWND hWnd) {
    if (IsDarkModePreferred() && !IsHighContrast())
        return TrySetWindowTheme(hWnd, true);
    return S_OK;
}

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif

#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3
#endif

#ifndef DWMWA_NCRENDERING_POLICY
#define DWMWA_NCRENDERING_POLICY 2
#endif

#ifndef DWMNCRP_DISABLED
#define DWMNCRP_DISABLED 1
#endif

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

// Enables native Windows DWM Acrylic frosted backdrop blur or resets to 100% clear transparency.
// When enable == true: applies DWM system backdrop Acrylic (DWMSBT_TRANSIENTWINDOW on Win11).
// When enable == false: resets DWM backdrop to DWMSBT_NONE with extended frame for 100% clear transparency.
// WS_EX_LAYERED is strictly stripped to guarantee 100% hit testing across the entire screen at any DPI scaling.
// WS_EX_NOREDIRECTIONBITMAP is enforced to eliminate stale GDI redirection buffers on resize and blur.
inline HRESULT TrySetWindowBackdrop(HWND hWnd, bool enable) {
    DWORD build = GetBuildNumber();

    // 1. Permanently strip WS_EX_LAYERED to eliminate 125% DPI hit-test dead zones
    // and ensure WS_EX_NOREDIRECTIONBITMAP is present
    LONG exStyle = ::GetWindowLong(hWnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_LAYERED) || !(exStyle & WS_EX_NOREDIRECTIONBITMAP)) {
        exStyle &= ~WS_EX_LAYERED;
        exStyle |= WS_EX_NOREDIRECTIONBITMAP;
        ::SetWindowLong(hWnd, GWL_EXSTYLE, exStyle);
    }

    // 2. Permanently disable DWM non-client rendering of native caption buttons
    int ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    // 3. Strip WS_SYSMENU, WS_MINIMIZEBOX, WS_MAXIMIZEBOX so DWM never draws native caption buttons
    DWORD currentStyle = ::GetWindowLong(hWnd, GWL_STYLE);
    if (currentStyle & WS_SYSMENU) {
        currentStyle &= ~(WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        ::SetWindowLong(hWnd, GWL_STYLE, currentStyle);
    }

    if (build >= 22621) {
        MARGINS margins = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        int backdropType = enable ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
        DwmSetWindowAttribute(hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    }

    // Flush frame and trigger redraw
    ::SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ::RedrawWindow(hWnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);

    return S_OK;
}

