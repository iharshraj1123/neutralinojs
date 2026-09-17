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

// Enables native Windows DWM Acrylic frosted backdrop blur or resets to 100% clear transparency.
// When enable == true: applies DWM system backdrop Acrylic (DWMSBT_TRANSIENTWINDOW on Win11 / ACCENT_ENABLE_ACRYLICBLURBEHIND on Win10).
// When enable == false: resets DWM backdrop to DWMSBT_NONE, resets frame margins to {1, 1, 1, 1}, restores WS_EX_LAYERED, and applies ACCENT_ENABLE_TRANSPARENTGRADIENT for 100% clear transparency.
inline HRESULT TrySetWindowBackdrop(HWND hWnd, bool enable) {
    DWORD build = GetBuildNumber();

    // 1. Permanently disable DWM non-client rendering of native caption buttons
    int ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    // 2. Strip WS_SYSMENU, WS_MINIMIZEBOX, WS_MAXIMIZEBOX so DWM never draws native caption buttons
    DWORD currentStyle = ::GetWindowLong(hWnd, GWL_STYLE);
    if (currentStyle & WS_SYSMENU) {
        currentStyle &= ~(WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        ::SetWindowLong(hWnd, GWL_STYLE, currentStyle);
    }

    if (enable) {
        // --- ACRYLIC FROSTED BLUR (blur > 0) ---
        if (build >= 22621) {
            // Remove WS_EX_LAYERED so DWM system backdrop can take effect on Win11 22H2+
            LONG exStyle = ::GetWindowLong(hWnd, GWL_EXSTYLE);
            ::SetWindowLong(hWnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);

            MARGINS margins = {-1, -1, -1, -1};
            DwmExtendFrameIntoClientArea(hWnd, &margins);

            int backdropType = DWMSBT_TRANSIENTWINDOW; // 3 = Acrylic
            DwmSetWindowAttribute(hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        }

        // Apply SetWindowCompositionAttribute Acrylic for Windows 10 and fallback
        HMODULE hUser32 = ::GetModuleHandleA("user32.dll");
        if (hUser32) {
            typedef enum _ACCENT_STATE {
                ACCENT_DISABLED = 0,
                ACCENT_ENABLE_GRADIENT = 1,
                ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
                ACCENT_ENABLE_BLURBEHIND = 3,
                ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
                ACCENT_ENABLE_HOSTBACKDROP = 5
            } ACCENT_STATE;

            typedef struct _ACCENT_POLICY {
                ACCENT_STATE AccentState;
                int AccentFlags;
                int GradientColor;
                int AnimationId;
            } ACCENT_POLICY;

            typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
                int Attribute;
                void* Data;
                int SizeOfData;
            } WINDOWCOMPOSITIONATTRIBDATA;

            typedef BOOL (WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
            pfnSetWindowCompositionAttribute pSet = 
                (pfnSetWindowCompositionAttribute)::GetProcAddress(hUser32, "SetWindowCompositionAttribute");

            if (pSet) {
                ACCENT_POLICY policy;
                policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
                policy.AccentFlags = 2; // draw all borders
                policy.GradientColor = 0x01181818; // subtle dark tint
                policy.AnimationId = 0;

                WINDOWCOMPOSITIONATTRIBDATA data;
                data.Attribute = 19; // WCA_ACCENT_POLICY
                data.Data = &policy;
                data.SizeOfData = sizeof(policy);
                pSet(hWnd, &data);
            }
        }
    } else {
        // --- 100% CRYSTAL-CLEAR TRANSPARENCY (blur == 0) ---
        if (build >= 22621) {
            // Reset DWM system backdrop to NONE
            int backdropType = DWMSBT_NONE;
            DwmSetWindowAttribute(hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));

            // Reset extended frame margins so DWM stops drawing the solid non-client frame inside client area
            MARGINS margins = {1, 1, 1, 1};
            DwmExtendFrameIntoClientArea(hWnd, &margins);
        }

        // Restore WS_EX_LAYERED so WebView2 DirectComposition renders transparently to desktop
        LONG exStyle = ::GetWindowLong(hWnd, GWL_EXSTYLE);
        ::SetWindowLong(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

        // Apply SetWindowCompositionAttribute ACCENT_ENABLE_TRANSPARENTGRADIENT for pure transparency
        HMODULE hUser32 = ::GetModuleHandleA("user32.dll");
        if (hUser32) {
            typedef enum _ACCENT_STATE {
                ACCENT_DISABLED = 0,
                ACCENT_ENABLE_GRADIENT = 1,
                ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
                ACCENT_ENABLE_BLURBEHIND = 3,
                ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
                ACCENT_ENABLE_HOSTBACKDROP = 5
            } ACCENT_STATE;

            typedef struct _ACCENT_POLICY {
                ACCENT_STATE AccentState;
                int AccentFlags;
                int GradientColor;
                int AnimationId;
            } ACCENT_POLICY;

            typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
                int Attribute;
                void* Data;
                int SizeOfData;
            } WINDOWCOMPOSITIONATTRIBDATA;

            typedef BOOL (WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
            pfnSetWindowCompositionAttribute pSet = 
                (pfnSetWindowCompositionAttribute)::GetProcAddress(hUser32, "SetWindowCompositionAttribute");

            if (pSet) {
                ACCENT_POLICY policy;
                policy.AccentState = ACCENT_ENABLE_TRANSPARENTGRADIENT;
                policy.AccentFlags = 2; // draw all borders
                policy.GradientColor = 0x00000000;
                policy.AnimationId = 0;

                WINDOWCOMPOSITIONATTRIBDATA data;
                data.Attribute = 19; // WCA_ACCENT_POLICY
                data.Data = &policy;
                data.SizeOfData = sizeof(policy);
                pSet(hWnd, &data);
            }
        }
    }

    // Flush frame and trigger redraw
    ::SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ::RedrawWindow(hWnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);

    return S_OK;
}

