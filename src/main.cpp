#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#include <strsafe.h>
#include <dbt.h>
#include <initguid.h>
#include <devguid.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_TRAY_ICON         (WM_USER + 101)
#define WM_APP_DPI_UPDATE    (WM_USER + 102)
#define WM_APP_BAT_UPDATE    (WM_USER + 103)
#define WM_APP_HEADSET_UPDATE (WM_USER + 104)
#define WM_APP_SHOW_STATUS   (WM_USER + 105)

#define IDM_HEADER          2001
#define IDM_BATTERY         2002
#define IDM_DPI             2003
#define IDM_AUTORUN         2004
#define IDM_RECONNECT       2005
#define IDM_EXIT            2006
#define IDM_HEADSET         2007

#define TIMER_OSD_HIDE      3001
#define TIMER_OSD_FADE      3002

#define OSD_KEY_COLOR       RGB(255, 0, 255) // Magenta transparent key

// Global State
static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hOsdWnd = NULL;
static NOTIFYICONDATAW g_nid = {0};
static HANDLE g_hHidThread = NULL;
static HANDLE g_hHeadsetThread = NULL;
static HANDLE g_hStopEvent = NULL;

static volatile LONG g_battery = 100;
static volatile LONG g_headsetBattery = -1;
static volatile LONG g_headsetConnected = FALSE;
static volatile LONG g_headsetCharging = FALSE;
static volatile LONG g_dpiLevel = 0;
static volatile LONG g_dpiX = 0;
static volatile LONG g_dpiY = 0;
static WCHAR g_osdTextLine1[64] = L"ATK Tray";
static WCHAR g_osdTextLine2[64] = L"DPI --";
static BYTE g_osdAlpha = 0;

static UINT g_uTaskbarRestartMsg = 0;
static const WCHAR* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR* APP_NAME = L"ATK-Tray";
static WCHAR g_detectedModel[64] = L"ATK device";
static volatile bool g_deviceConnected = false;

static void LogMessage(const WCHAR* format, ...) {
    WCHAR appData[MAX_PATH] = {0};
    DWORD appDataLen = GetEnvironmentVariableW(L"LOCALAPPDATA", appData, ARRAYSIZE(appData));
    if (appDataLen == 0 || appDataLen >= ARRAYSIZE(appData)) {
        if (GetTempPathW(ARRAYSIZE(appData), appData) == 0) return;
    }

    WCHAR logDir[MAX_PATH] = {0};
    WCHAR logPath[MAX_PATH] = {0};
    if (FAILED(StringCchPrintfW(logDir, ARRAYSIZE(logDir), L"%s\\ATK Tray", appData)) ||
        FAILED(StringCchPrintfW(logPath, ARRAYSIZE(logPath), L"%s\\atk-tray.log", logDir))) {
        return;
    }
    CreateDirectoryW(logDir, NULL);

    WCHAR message[768] = {0};
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(message, ARRAYSIZE(message), format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    WCHAR line[1024] = {0};
    if (FAILED(StringCchPrintfW(line, ARRAYSIZE(line),
        L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, message))) {
        return;
    }

    char utf8[2048] = {0};
    int byteCount = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, ARRAYSIZE(utf8), NULL, NULL);
    if (byteCount <= 1) return;

    HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, utf8, (DWORD)(byteCount - 1), &written, NULL);
        CloseHandle(file);
    }
}

static LONG WINAPI AtkUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    DWORD code = exceptionInfo && exceptionInfo->ExceptionRecord
        ? exceptionInfo->ExceptionRecord->ExceptionCode : 0;
    PVOID address = exceptionInfo && exceptionInfo->ExceptionRecord
        ? exceptionInfo->ExceptionRecord->ExceptionAddress : NULL;
    LogMessage(L"未处理异常：代码 0x%08lX，地址 %p", code, address);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void UpdateTrayTooltip();
static void UpdateTrayIcon(int battery);
static void ShowOsdNotification(int dpiLevel, int dpiX, int battery);
static void UpdateHeadsetState(bool connected, int battery, bool charging);
static bool IsAutoRunEnabled();
static void SetAutoRun(bool enable);

// 3x5 font for 16x16: 3 bits per row, 5 rows
static const uint8_t FONT_3X5[10][5] = {
    { 0x07, 0x05, 0x05, 0x05, 0x07 }, // '0' (111, 101, 101, 101, 111)
    { 0x02, 0x06, 0x02, 0x02, 0x07 }, // '1' (010, 110, 010, 010, 111)
    { 0x07, 0x01, 0x07, 0x04, 0x07 }, // '2' (111, 001, 111, 100, 111)
    { 0x07, 0x01, 0x07, 0x01, 0x07 }, // '3' (111, 001, 111, 001, 111)
    { 0x05, 0x05, 0x07, 0x01, 0x01 }, // '4' (101, 101, 111, 001, 001)
    { 0x07, 0x04, 0x07, 0x01, 0x07 }, // '5' (111, 100, 111, 001, 111)
    { 0x07, 0x04, 0x07, 0x05, 0x07 }, // '6' (111, 100, 111, 101, 111)
    { 0x07, 0x01, 0x02, 0x02, 0x02 }, // '7' (111, 001, 010, 010, 010)
    { 0x07, 0x05, 0x07, 0x05, 0x07 }, // '8' (111, 101, 111, 101, 111)
    { 0x07, 0x05, 0x07, 0x01, 0x07 }  // '9' (111, 101, 111, 001, 111)
};

// 5x9 font for 20x20 and 24x24: 5 bits per row, 9 rows
static const uint16_t FONT_5X9[10][9] = {
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // '0'
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, // '1'
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10, 0x1F }, // '2'
    { 0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x01, 0x01, 0x1E }, // '3'
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02, 0x02, 0x02 }, // '4'
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x01, 0x01, 0x11, 0x0E }, // '5'
    { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11, 0x0E }, // '6'
    { 0x1F, 0x01, 0x02, 0x02, 0x04, 0x04, 0x08, 0x08, 0x08 }, // '7'
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E }, // '8'
    { 0x0E, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x01, 0x0E }  // '9'
};

// 4x9 font for '0' in '100' at 20x20 / 24x24
static const uint8_t FONT_4X9_0[9] = {
    0x06, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x06
};

static inline void PutPixelARGB(uint32_t* pPixels, int size, int x, int y, uint32_t color) {
    if (x >= 0 && x < size && y >= 0 && y < size) {
        pPixels[y * size + x] = color;
    }
}

static int GetTrayIconSize(HWND hWnd) {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
    typedef int (WINAPI *pfnGetSystemMetricsForDpi)(int, UINT);
    pfnGetDpiForWindow fnGetDpiForWindow = (pfnGetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
    pfnGetSystemMetricsForDpi fnGetSystemMetricsForDpi = (pfnGetSystemMetricsForDpi)GetProcAddress(hUser, "GetSystemMetricsForDpi");

    int sz = 0;
    if (fnGetDpiForWindow && fnGetSystemMetricsForDpi && hWnd) {
        UINT dpi = fnGetDpiForWindow(hWnd);
        if (dpi > 0) {
            sz = fnGetSystemMetricsForDpi(SM_CXSMICON, dpi);
        }
    }
    if (sz <= 0) {
        sz = GetSystemMetrics(SM_CXSMICON);
    }
    if (sz <= 0) sz = 16;
    return sz;
}

static bool IsSystemDarkTheme() {
    DWORD val = 0, size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS) {
        return (val == 0);
    }
    return true; // Default to dark theme
}

// Generate Win11-style slim battery icon.
// Rendered at 4x supersampling then box-downsampled, so edges and digits
// come out anti-aliased instead of fat/pixelated in the tray.
static HICON CreateLegacyBatteryIcon(int battery) {
    if (battery < 0) battery = 0;
    if (battery > 100) battery = 100;
    int size = GetTrayIconSize(g_hMainWnd);
    if (size <= 0) size = 16;
    const int SS = 4;
    const int W = size * SS;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -W; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = NULL;
    HBITMAP hbmColor = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    if (!hbmColor || !pvBits) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }

    uint32_t* hi = (uint32_t*)pvBits;
    memset(hi, 0, W * W * sizeof(uint32_t));

    bool isDark = IsSystemDarkTheme();

    // A layered battery face stays readable at tray size on both taskbar themes.
    const uint32_t c_fill  = battery <= 10 ? 0xFFE25555 : (battery <= 25 ? 0xFFF2B84B : 0xFF35D58A);
    const uint32_t c_cavity = isDark ? 0xFF25303B : 0xFFE9EEF2;
    const uint32_t c_frame = isDark ? 0xFFF5F7FA : 0xFF273442;
    const uint32_t c_digit = 0xFF102019;

    // --- hi-res drawing helpers ---
    auto HiPixel = [&](int x, int y, uint32_t col) {
        if (x >= 0 && x < W && y >= 0 && y < W) hi[y * W + x] = col;
    };
    auto FillRoundRect = [&](int x0, int y0, int x1, int y1, uint32_t col, int rad) {
        if (rad > (x1 - x0) / 2) rad = (x1 - x0) / 2;
        if (rad > (y1 - y0) / 2) rad = (y1 - y0) / 2;
        if (rad < 0) rad = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                int dx = (x < x0 + rad) ? (x0 + rad - x) : (x > x1 - rad) ? (x - (x1 - rad)) : 0;
                int dy = (y < y0 + rad) ? (y0 + rad - y) : (y > y1 - rad) ? (y - (y1 - rad)) : 0;
                if (dx * dx + dy * dy <= rad * rad) HiPixel(x, y, col);
            }
    };

    // Restored full-size battery geometry:
    // Strictly aligned to 1x screen pixel boundaries for 100% crisp, razor-sharp edges,
    // utilizing the full icon area (pad_y=1 for 16x16, filling height 14px and width 16px).
    int pad_y = (size < 20) ? 2 : ((size <= 24) ? 2 : 3);
    int tip_w = (size < 20) ? 2 : ((size <= 24) ? 3 : 4);
    int tip_h = (size < 20) ? 6 : (size / 3);
    int tip_y = (size - tip_h) / 2;

    int body_x0 = (size < 20 ? 1 : 2) * SS;
    int body_x1 = (size - 1 - tip_w) * SS - 1;
    int body_y0 = pad_y * SS;
    int body_y1 = (size - 1 - pad_y) * SS + (SS - 1);

    int frame_t = 1 * SS; // Exact 1.0 screen pixel border (razor-sharp)
    int radius  = (size < 20 ? 1 : 2) * SS;

    FillRoundRect(body_x0, body_y0, body_x1, body_y1, c_frame, radius);
    FillRoundRect(body_x0 + frame_t, body_y0 + frame_t,
                  body_x1 - frame_t, body_y1 - frame_t, c_cavity,
                  (radius > frame_t ? radius - frame_t : 0));
    // The colored face makes the exact percentage visible without relying on the tooltip.
    int inner_x0 = body_x0 + frame_t;
    int inner_x1 = body_x1 - frame_t;
    int inner_y0 = body_y0 + frame_t;
    int inner_y1 = body_y1 - frame_t;
    int inner_width = inner_x1 - inner_x0 + 1;
    int fill_x1 = inner_x0 + (inner_width * battery) / 100 - 1;
    if (battery > 0) {
        FillRoundRect(inner_x0, inner_y0, fill_x1, inner_y1, c_fill,
                      (battery >= 90 && radius > frame_t) ? radius - frame_t : 0);
    }

    // Positive terminal tip (fully aligned to screen pixels):
    int tip_x0 = (size - tip_w) * SS;
    int tip_x1 = size * SS - 1;
    int tip_y0 = tip_y * SS;
    int tip_y1 = (tip_y + tip_h) * SS - 1;
    FillRoundRect(tip_x0, tip_y0, tip_x1, tip_y1, c_frame, SS);

    int in_x0 = body_x0 + frame_t;
    int in_x1 = body_x1 - frame_t;
    int in_y0 = body_y0 + frame_t;
    int in_w  = in_x1 - in_x0 + 1;
    int in_h  = (body_y1 - frame_t) - in_y0 + 1;

    char s[8];
    snprintf(s, sizeof(s), "%d", battery);
    int len = (int)strlen(s);

    {
        // font metrics (font pixels)
        const int fw = (size < 20) ? 3 : 5;
        const int fh = (size < 20) ? 5 : 9;

        int gap_fp = (len == 1) ? 0 : 1; // 1 font-pixel gap between digits

        // Set font height to ~70% of inner cavity height (step up to scale = 6, 7.5px)
        int target_h = in_h * 70 / 100;
        int scale = target_h / fh;
        if (scale < 1) scale = 1;
        int bold_w = 1;

        auto CalcTextWidth = [&](int sc, int bw) -> int {
            if (battery == 100 && size < 20) {
                return (sc + bw) + (gap_fp * sc) + 2 * (3 * sc + bw) + (gap_fp * sc);
            } else if (battery == 100) {
                return (2 * sc + bw) + (gap_fp * sc) + 2 * (4 * sc + bw) + (gap_fp * sc);
            } else {
                return len * (fw * sc + bw) + (len - 1) * (gap_fp * sc);
            }
        };

        while (scale > 1 && CalcTextWidth(scale, bold_w) > in_w) {
            --scale;
        }

        int text_h = fh * scale;
        int text_w = CalcTextWidth(scale, bold_w);
        int sx = in_x0 + (in_w - text_w) / 2;
        int sy = in_y0 + (in_h - text_h) / 2;

        auto DrawDigitScaled = [&](const uint16_t* rows, int fw, int fh,
                                   int x0, int y0, int scale) {
            for (int r = 0; r < fh; ++r) {
                for (int c = 0; c < fw; ++c) {
                    if (!((rows[r] >> (fw - 1 - c)) & 1)) continue;
                    for (int dy = 0; dy < scale; ++dy) {
                        for (int dx = 0; dx < scale + bold_w; ++dx) {
                            int px = x0 + c * scale + dx;
                            int py = y0 + r * scale + dy;
                            HiPixel(px, py, c_digit);
                        }
                    }
                }
            }
        };

        if (battery == 100 && size < 20) {
            // "00" with a leading 1 bar, 3x5 font
            int cur = sx;
            for (int t = 0; t < scale + bold_w; ++t)
                for (int r = 0; r < text_h; ++r)
                    HiPixel(cur + t, sy + r, c_digit);
            cur += scale + bold_w + gap_fp * scale;
            uint16_t rows5[5];
            for (int r = 0; r < 5; ++r) rows5[r] = FONT_3X5[0][r];
            DrawDigitScaled(rows5, 3, 5, cur, sy, scale);
            cur += 3 * scale + bold_w + gap_fp * scale;
            DrawDigitScaled(rows5, 3, 5, cur, sy, scale);
        } else if (battery == 100) {
            // "00" with a leading 1 bar, 4x9 font
            int cur = sx;
            for (int t = 0; t < 2 * scale + bold_w; ++t)
                for (int r = 0; r < text_h; ++r)
                    HiPixel(cur + t, sy + r, c_digit);
            cur += 2 * scale + bold_w + gap_fp * scale;
            uint16_t rows9[9];
            for (int r = 0; r < 9; ++r) rows9[r] = FONT_4X9_0[r];
            DrawDigitScaled(rows9, 4, 9, cur, sy, scale);
            cur += 4 * scale + bold_w + gap_fp * scale;
            DrawDigitScaled(rows9, 4, 9, cur, sy, scale);
        } else {
            int cur = sx;
            for (int i = 0; i < len; ++i) {
                int d = s[i] - '0';
                if (size < 20) {
                    uint16_t rows5[5];
                    for (int r = 0; r < 5; ++r) rows5[r] = FONT_3X5[d][r];
                    DrawDigitScaled(rows5, 3, 5, cur, sy, scale);
                } else {
                    DrawDigitScaled(FONT_5X9[d], 5, 9, cur, sy, scale);
                }
                cur += fw * scale + bold_w + gap_fp * scale;
            }
        }
    }

    // --- box downsample SSx -> 1x (alpha-weighted average = anti-aliasing) ---
    BITMAPINFO bomi = {0};
    bomi.bmiHeader = bmi.bmiHeader;
    bomi.bmiHeader.biWidth = size;
    bomi.bmiHeader.biHeight = -size;
    void* pvOut = NULL;
    HBITMAP hbmOut = CreateDIBSection(hdcMem, &bomi, DIB_RGB_COLORS, &pvOut, NULL, 0);
    if (!hbmOut || !pvOut) {
        DeleteObject(hbmColor);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }
    uint32_t* out = (uint32_t*)pvOut;
    const int SS2 = SS * SS;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t sumA = 0, sumR = 0, sumG = 0, sumB = 0;
            for (int sy = 0; sy < SS; ++sy) {
                const uint32_t* row = hi + (y * SS + sy) * W + x * SS;
                for (int sx = 0; sx < SS; ++sx) {
                    uint32_t c = row[sx];
                    uint32_t a = (c >> 24) & 0xFF;
                    if (a > 0) {
                        sumA += a;
                        sumR += (c >> 16) & 0xFF;
                        sumG += (c >> 8) & 0xFF;
                        sumB += (c & 0xFF);
                    }
                }
            }
            uint32_t a = sumA / SS2;
            if (a < 8) {
                out[y * size + x] = 0;
            } else {
                uint32_t r = sumR / SS2;
                uint32_t g = sumG / SS2;
                uint32_t b = sumB / SS2;
                out[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    // Windows monochrome 1bpp mask: 1 = Transparent, 0 = Opaque.
    // Row width must be WORD (2-byte) aligned.
    int maskPitch = ((size + 15) / 16) * 2;
    BYTE maskBits[256] = {0};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t px = out[y * size + x];
            uint8_t a = (px >> 24) & 0xFF;
            if (a < 32) {
                maskBits[y * maskPitch + (x / 8)] |= (1 << (7 - (x % 8)));
            }
        }
    }
    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, maskBits);

    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmOut;
    ii.hbmMask = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmOut);
    DeleteObject(hbmMask);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    return hIcon;
}

// Draw the tray icon at its final pixel size. The percentage uses a compact
// 3x5 bitmap font so Windows 10 does not blur a scaled TrueType font.
static HICON CreateBatteryIcon(int battery) {
    if (battery < 0) battery = 0;
    if (battery > 100) battery = 100;

    int size = GetTrayIconSize(g_hMainWnd);
    if (size < 16) size = 16;
    if (size > 64) size = 64;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP colorBitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!colorBitmap || !bits) {
        if (colorBitmap) DeleteObject(colorBitmap);
        DeleteDC(dc);
        ReleaseDC(NULL, screen);
        return NULL;
    }

    uint32_t* pixels = (uint32_t*)bits;
    memset(pixels, 0, (size_t)size * size * sizeof(uint32_t));
    const bool dark = IsSystemDarkTheme();
    // Fill the whole cavity by percentage. The fill colors are intentionally
    // deep and saturated so the white pixel font remains highly legible.
    const uint32_t frame = dark ? 0xFFFFFFFF : 0xFF111111;
    const uint32_t cavity = 0xFF101820;
    const uint32_t fill = battery <= 10 ? 0xFF9B1025
        : (battery <= 25 ? 0xFF956000 : 0xFF007A3D);

    auto put = [&](int x, int y, uint32_t color) {
        if (x >= 0 && x < size && y >= 0 && y < size) pixels[y * size + x] = color;
    };
    auto rect = [&](int left, int top, int right, int bottom, uint32_t color) {
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) put(x, y, color);
        }
    };

    const int tipWidth = size >= 24 ? 3 : 2;
    const int bodyLeft = size >= 24 ? 2 : (size >= 20 ? 1 : 0);
    const int bodyRight = size - tipWidth - 2;
    const int bodyTop = size >= 24 ? 3 : (size >= 20 ? 2 : 1);
    const int bodyBottom = size - bodyTop - 1;
    const int tipHeight = size >= 24 ? size / 3 : (size >= 20 ? 7 : 5);
    const int tipTop = (size - tipHeight) / 2;
    const int tipLeft = size - tipWidth;

    rect(bodyLeft, bodyTop, bodyRight, bodyBottom, frame);
    rect(bodyLeft + 1, bodyTop + 1, bodyRight - 1, bodyBottom - 1, cavity);

    const int innerLeft = bodyLeft + 1;
    const int innerRight = bodyRight - 1;
    const int innerTop = bodyTop + 1;
    const int innerBottom = bodyBottom - 1;
    const int innerWidth = innerRight - innerLeft + 1;
    int fillWidth = (innerWidth * battery + 99) / 100;
    if (battery == 0) fillWidth = 0;
    if (fillWidth > 0) {
        rect(innerLeft, innerTop, innerLeft + fillWidth - 1, innerBottom, fill);
    }
    rect(tipLeft, tipTop, size - 1, tipTop + tipHeight - 1, frame);

    // Keep the number inside the battery, but draw it as hard 1px pixels.
    // The 3x5 font is deliberately compact: even "100" fits in a 16px icon.
    char text[8] = {0};
    snprintf(text, sizeof(text), "%d", battery);
    const int digitWidth = 3;
    const int digitHeight = 5;
    const int digitGap = 1;
    const int textWidth = (int)strlen(text) * digitWidth +
        ((int)strlen(text) - 1) * digitGap;
    const int textLeft = innerLeft + ((innerWidth - textWidth) > 0
        ? (innerWidth - textWidth) / 2 : 0);
    const int textTop = innerTop + ((innerBottom - innerTop + 1 - digitHeight) > 0
        ? (innerBottom - innerTop + 1 - digitHeight) / 2 : 0);
    const uint32_t textColor = 0xFFFFFFFF;
    int cursor = textLeft;
    for (size_t i = 0; i < strlen(text); ++i) {
        const int digit = text[i] - '0';
        for (int row = 0; row < digitHeight; ++row) {
            for (int col = 0; col < digitWidth; ++col) {
                if ((FONT_3X5[digit][row] & (1 << (digitWidth - 1 - col))) != 0) {
                    put(cursor + col, textTop + row, textColor);
                }
            }
        }
        cursor += digitWidth + digitGap;
    }

    // One-pixel clipped corners keep the silhouette sharp without blurred
    // anti-aliasing at the exact size used by the taskbar.
    if (size <= 20) {
        put(bodyLeft, bodyTop, 0);
        put(bodyLeft, bodyBottom, 0);
        put(bodyRight, bodyTop, 0);
        put(bodyRight, bodyBottom, 0);
    }

    const int maskPitch = ((size + 15) / 16) * 2;
    const int maskBytes = maskPitch * size;
    BYTE* maskBits = (BYTE*)calloc((size_t)maskBytes, 1);
    HBITMAP maskBitmap = NULL;
    HICON icon = NULL;
    if (maskBits) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (((pixels[y * size + x] >> 24) & 0xFF) == 0) {
                    maskBits[y * maskPitch + x / 8] |= (BYTE)(1 << (7 - (x % 8)));
                }
            }
        }
        maskBitmap = CreateBitmap(size, size, 1, 1, maskBits);
        if (maskBitmap) {
            ICONINFO info = {0};
            info.fIcon = TRUE;
            info.hbmColor = colorBitmap;
            info.hbmMask = maskBitmap;
            icon = CreateIconIndirect(&info);
        }
        free(maskBits);
    }

    if (maskBitmap) DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return icon;
}

#if 0
static void LegacyUpdateTrayTooltip() {
    if (g_deviceConnected) {
        StringCchPrintfW(
            g_nid.szTip,
            ARRAYSIZE(g_nid.szTip),
            L"%s\n电量: %d%%\nDPI: %d (第 %d 档)",
            g_detectedModel,
            g_battery,
            g_dpiX,
            g_dpiLevel
        );
    } else {
        StringCchPrintfW(
            g_nid.szTip,
            ARRAYSIZE(g_nid.szTip),
            L"ATK Tray\n等待 ATK 设备连接..."
        );
    }
}

#endif
static void LegacyUpdateTrayTooltip() {
    if (g_deviceConnected) {
        StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
            g_headsetConnected && g_headsetBattery >= 0
                ? L"ATK Tray\n%s\n鼠标电量：%d%%\n耳机电量：%d%%"
                : L"ATK Tray\n%s\n鼠标电量：%d%%",
            g_detectedModel, g_battery, g_headsetBattery);
    } else if (g_headsetConnected && g_headsetBattery >= 0) {
        StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
            L"ATK Tray\n耳机电量：%d%%", g_headsetBattery);
    } else {
        StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"ATK Tray\n等待 ATK 设备连接...");
    }
}

static void UpdateTrayTooltip() {
    const bool mouseConnected = g_deviceConnected;
    const bool headsetConnected = InterlockedCompareExchange(&g_headsetConnected, 0, 0) != FALSE;
    const int headsetBattery = (int)InterlockedCompareExchange(&g_headsetBattery, 0, 0);
    const int dpiX = (int)InterlockedCompareExchange(&g_dpiX, 0, 0);
    const int dpiLevel = (int)InterlockedCompareExchange(&g_dpiLevel, 0, 0);
    if (mouseConnected && headsetConnected && headsetBattery >= 0) {
        StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
            L"ATK Tray\n\u9F20\u6807\u7535\u91CF: %d%%\nDPI: %d (\u7b2c %d \u6863)\n\u8033\u673A\u7535\u91CF: %d%%",
            (int)g_battery, dpiX > 0 ? dpiX : 0, dpiLevel > 0 ? dpiLevel : 0, headsetBattery);
    } else if (mouseConnected) {
        if (dpiX > 0) {
            StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
                L"ATK Tray\n%s\n\u9F20\u6807\u7535\u91CF: %d%%\nDPI: %d (\u7b2c %d \u6863)",
                g_detectedModel, (int)g_battery, dpiX, dpiLevel);
        } else {
            StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
                L"ATK Tray\n%s\n\u9F20\u6807\u7535\u91CF: %d%%\nDPI: --",
                g_detectedModel, (int)g_battery);
        }
    } else if (headsetConnected && headsetBattery >= 0) {
        StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
            L"ATK Tray\n\u8033\u673A\u7535\u91CF: %d%%", headsetBattery);
    } else {
        StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip),
            L"ATK Tray\n\u7B49\u5F85 ATK \u8BBE\u5907\u8FDE\u63A5...");
    }
}

static void UpdateHeadsetState(bool connected, int battery, bool charging) {
    InterlockedExchange(&g_headsetConnected, connected ? TRUE : FALSE);
    InterlockedExchange(&g_headsetBattery, connected ? battery : -1);
    InterlockedExchange(&g_headsetCharging, charging ? TRUE : FALSE);
}

static void UpdateTrayIcon(int battery) {
    HICON hNewIcon = CreateBatteryIcon(battery);
    if (hNewIcon) {
        if (g_nid.hIcon) {
            DestroyIcon(g_nid.hIcon);
        }
        g_nid.hIcon = hNewIcon;
    }
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    UpdateTrayTooltip();
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// OSD Floating Window Procedure with Double-Buffering and Transparency
static LRESULT CALLBACK OsdWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            // Fill transparent color key
            HBRUSH hBrKey = CreateSolidBrush(OSD_KEY_COLOR);
            FillRect(memDC, &rc, hBrKey);
            DeleteObject(hBrKey);

            // Modern Dark Floating Pill
            RECT rcBox = rc;
            InflateRect(&rcBox, -2, -2);
            HBRUSH hBg = CreateSolidBrush(RGB(24, 26, 32));
            HPEN hBorder = CreatePen(PS_SOLID, 1, RGB(70, 75, 90));
            HGDIOBJ oldBr = SelectObject(memDC, hBg);
            HGDIOBJ oldPen = SelectObject(memDC, hBorder);
            RoundRect(memDC, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom, 22, 22);

            SetBkMode(memDC, TRANSPARENT);

            // Level + DPI Value (Prominent Bold Font)
            HFONT hFontBig = CreateFontW(
                -24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI"
            );
            HGDIOBJ oldFont = SelectObject(memDC, hFontBig);
            SetTextColor(memDC, RGB(255, 255, 255));
            RECT rcTop = rcBox;
            rcTop.bottom = rcBox.top + 42;
            DrawTextW(memDC, g_osdTextLine1, -1, &rcTop, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Subtitle (Model + Battery in Cyan)
            HFONT hFontSub = CreateFontW(
                -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI"
            );
            SelectObject(memDC, hFontSub);
            SetTextColor(memDC, RGB(130, 215, 255));
            RECT rcBot = rcBox;
            rcBot.top = rcBox.top + 40;
            DrawTextW(memDC, g_osdTextLine2, -1, &rcBot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBr);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            DeleteObject(hFontBig);
            DeleteObject(hFontSub);
            DeleteObject(hBorder);
            DeleteObject(hBg);

            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            if (wParam == TIMER_OSD_HIDE) {
                KillTimer(hWnd, TIMER_OSD_HIDE);
                SetTimer(hWnd, TIMER_OSD_FADE, 16, NULL);
            } else if (wParam == TIMER_OSD_FADE) {
                if (g_osdAlpha > 15) {
                    g_osdAlpha -= 15;
                    SetLayeredWindowAttributes(hWnd, OSD_KEY_COLOR, g_osdAlpha, LWA_COLORKEY | LWA_ALPHA);
                } else {
                    KillTimer(hWnd, TIMER_OSD_FADE);
                    ShowWindow(hWnd, SW_HIDE);
                    g_osdAlpha = 0;
                }
            }
            return 0;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

#if 0
static void LegacyShowOsdNotification(int dpiLevel, int dpiX, int battery) {
    if (!g_hOsdWnd) return;

    StringCchPrintfW(g_osdTextLine1, ARRAYSIZE(g_osdTextLine1), L"第 %d 档  DPI %d", dpiLevel, dpiX);
    StringCchPrintfW(g_osdTextLine2, ARRAYSIZE(g_osdTextLine2), L"%s  |  电量 %d%%", g_detectedModel, battery);

    RECT rcWork;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0)) {
        rcWork.left = 0;
        rcWork.top = 0;
        rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int w = 260;
    int h = 76;
    int x = rcWork.left + ((rcWork.right - rcWork.left) - w) / 2;
    int y = rcWork.bottom - h - 80;

    SetWindowPos(g_hOsdWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_osdAlpha = 240;
    SetLayeredWindowAttributes(g_hOsdWnd, OSD_KEY_COLOR, g_osdAlpha, LWA_COLORKEY | LWA_ALPHA);
    InvalidateRect(g_hOsdWnd, NULL, FALSE);
    UpdateWindow(g_hOsdWnd);

    KillTimer(g_hOsdWnd, TIMER_OSD_FADE);
    SetTimer(g_hOsdWnd, TIMER_OSD_HIDE, 1400, NULL);
}

#endif
static void LegacyShowOsdNotification(int dpiLevel, int dpiX, int battery) {
    (void)dpiLevel;
    (void)dpiX;
    if (!g_hOsdWnd) return;

    StringCchCopyW(g_osdTextLine1, ARRAYSIZE(g_osdTextLine1), L"ATK Tray");
    if (g_headsetConnected && g_headsetBattery >= 0) {
        StringCchPrintfW(g_osdTextLine2, ARRAYSIZE(g_osdTextLine2),
            L"鼠标 %d%%  |  耳机 %d%%%s", battery, g_headsetBattery,
            g_headsetCharging ? L"（充电中）" : L"");
    } else {
        StringCchPrintfW(g_osdTextLine2, ARRAYSIZE(g_osdTextLine2),
            L"%s  |  电量：%d%%", g_detectedModel, battery);
    }

    RECT rcWork;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0)) {
        rcWork.left = 0;
        rcWork.top = 0;
        rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int w = 380;
    const int h = 76;
    const int x = rcWork.left + ((rcWork.right - rcWork.left) - w) / 2;
    const int y = rcWork.bottom - h - 80;

    SetWindowPos(g_hOsdWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_osdAlpha = 240;
    SetLayeredWindowAttributes(g_hOsdWnd, OSD_KEY_COLOR, g_osdAlpha, LWA_COLORKEY | LWA_ALPHA);
    InvalidateRect(g_hOsdWnd, NULL, FALSE);
    UpdateWindow(g_hOsdWnd);

    KillTimer(g_hOsdWnd, TIMER_OSD_FADE);
    SetTimer(g_hOsdWnd, TIMER_OSD_HIDE, 1400, NULL);
}

#if 1
static void ShowOsdNotification(int dpiLevel, int dpiX, int battery) {
    if (!g_hOsdWnd) return;

    if (dpiX > 0) {
        StringCchPrintfW(g_osdTextLine1, ARRAYSIZE(g_osdTextLine1),
            L"DPI %d  ·  第 %d 档", dpiX, dpiLevel > 0 ? dpiLevel : 1);
    } else {
        StringCchCopyW(g_osdTextLine1, ARRAYSIZE(g_osdTextLine1), L"DPI --");
    }
    if (g_headsetConnected && g_headsetBattery >= 0) {
        StringCchPrintfW(g_osdTextLine2, ARRAYSIZE(g_osdTextLine2),
            L"\u9F20\u6807 %d%%  |  \u8033\u673A %d%%%s", battery, (int)g_headsetBattery,
            g_headsetCharging ? L" (\u5145\u7535\u4E2D)" : L"");
    } else {
        StringCchPrintfW(g_osdTextLine2, ARRAYSIZE(g_osdTextLine2),
            L"%s  |  \u9F20\u6807\u7535\u91CF: %d%%", g_detectedModel, battery);
    }

    RECT rcWork;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0)) {
        rcWork.left = 0;
        rcWork.top = 0;
        rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int w = 380;
    const int h = 76;
    const int x = rcWork.left + ((rcWork.right - rcWork.left) - w) / 2;
    const int y = rcWork.bottom - h - 80;

    SetWindowPos(g_hOsdWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_osdAlpha = 240;
    SetLayeredWindowAttributes(g_hOsdWnd, OSD_KEY_COLOR, g_osdAlpha, LWA_COLORKEY | LWA_ALPHA);
    InvalidateRect(g_hOsdWnd, NULL, FALSE);
    UpdateWindow(g_hOsdWnd);
    KillTimer(g_hOsdWnd, TIMER_OSD_FADE);
    SetTimer(g_hOsdWnd, TIMER_OSD_HIDE, 1400, NULL);
}
#endif

#if 0
// Legacy non-ATK HID implementation retained for reference only.
static bool FindLegacyNonAtkReportPath(WCHAR* outPath, DWORD maxLen, WCHAR* outModel, DWORD maxModelLen) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA devData = {0};
    devData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    bool found = false;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, i, &devData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, NULL, 0, &reqSize, NULL);
        if (reqSize == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!pDetail) continue;

        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, pDetail, reqSize, NULL, NULL)) {
            WCHAR lowerPath[MAX_PATH];
            StringCchCopyW(lowerPath, MAX_PATH, pDetail->DevicePath);
            _wcslwr_s(lowerPath, MAX_PATH);

            if (wcsstr(lowerPath, L"vid_24ae") && 
               (wcsstr(lowerPath, L"pid_1460") || wcsstr(lowerPath, L"pid_4660") || wcsstr(lowerPath, L"pid_1411") || wcsstr(lowerPath, L"pid_1410")) && 
                wcsstr(lowerPath, L"col09")) {
                StringCchCopyW(outPath, maxLen, pDetail->DevicePath);
                found = true;

                if (outModel && maxModelLen > 0) {
                    if (wcsstr(lowerPath, L"pid_1460")) {
                        StringCchCopyW(outModel, maxModelLen, L"Legacy mouse");
                    } else if (wcsstr(lowerPath, L"pid_4660")) {
                        StringCchCopyW(outModel, maxModelLen, L"Legacy mouse (wired)");
                    } else if (wcsstr(lowerPath, L"pid_1411")) {
                        StringCchCopyW(outModel, maxModelLen, L"Legacy mouse (wired)");
                    } else if (wcsstr(lowerPath, L"pid_1410")) {
                        StringCchCopyW(outModel, maxModelLen, L"Legacy mouse");
                    } else {
                        StringCchCopyW(outModel, maxModelLen, L"Legacy mouse");
                    }
                }

                free(pDetail);
                break;
            }
        }
        free(pDetail);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

#endif
// ATK mice expose their battery on an 8-byte feature report with ID 0x06.
static bool FindAtkBatteryPath(WCHAR* outPath, DWORD maxLen, WCHAR* outModel, DWORD maxModelLen) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA devData = {0};
    devData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    bool found = false;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, i, &devData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, NULL, 0, &reqSize, NULL);
        if (reqSize == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!pDetail) continue;

        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, pDetail, reqSize, NULL, NULL)) {
            WCHAR lowerPath[MAX_PATH];
            StringCchCopyW(lowerPath, MAX_PATH, pDetail->DevicePath);
            _wcslwr_s(lowerPath, MAX_PATH);

            if (wcsstr(lowerPath, L"vid_373b") && wcsstr(lowerPath, L"col06")) {
                StringCchCopyW(outPath, maxLen, pDetail->DevicePath);
                if (outModel && maxModelLen > 0) {
                    StringCchCopyW(outModel, maxModelLen, L"ATK device");
                    HANDLE hProduct = CreateFileW(
                        pDetail->DevicePath,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        OPEN_EXISTING,
                        0,
                        NULL
                    );
                    if (hProduct != INVALID_HANDLE_VALUE) {
                        WCHAR product[64] = {0};
                        if (HidD_GetProductString(hProduct, product, sizeof(product))) {
                            StringCchCopyW(outModel, maxModelLen, product);
                        }
                        CloseHandle(hProduct);
                    }
                }
                found = true;
                free(pDetail);
                break;
            }
        }
        free(pDetail);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

static bool FindAtk8KReceiverPath(WCHAR* outPath, DWORD maxLen,
    WCHAR* outModel, DWORD maxModelLen) {
    if (!outPath || maxLen == 0) return false;
    outPath[0] = L'\0';
    if (outModel && maxModelLen > 0) outModel[0] = L'\0';

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA devData = {0};
    devData.cbSize = sizeof(devData);
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, i, &devData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, NULL, 0, &reqSize, NULL);
        if (reqSize == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, detail,
                reqSize, NULL, NULL)) {
            WCHAR lowerPath[MAX_PATH] = {0};
            StringCchCopyW(lowerPath, ARRAYSIZE(lowerPath), detail->DevicePath);
            _wcslwr_s(lowerPath, ARRAYSIZE(lowerPath));
            if (wcsstr(lowerPath, L"vid_373b&pid_101b") &&
                wcsstr(lowerPath, L"col05")) {
                StringCchCopyW(outPath, maxLen, detail->DevicePath);
                if (outModel && maxModelLen > 0) {
                    StringCchCopyW(outModel, maxModelLen, L"ATK Mouse 8K Dongle");
                    HANDLE hProduct = CreateFileW(detail->DevicePath, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                    if (hProduct != INVALID_HANDLE_VALUE) {
                        WCHAR product[64] = {0};
                        if (HidD_GetProductString(hProduct, product, sizeof(product))) {
                            StringCchCopyW(outModel, maxModelLen, product);
                        }
                        CloseHandle(hProduct);
                    }
                }
                found = true;
                free(detail);
                break;
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

static BYTE Atk8KChecksum(const BYTE* payload) {
    BYTE sum = 0;
    for (int i = 0; i < 15; ++i) {
        sum = (BYTE)(sum + payload[i]);
    }
    return (BYTE)(0x55 - (BYTE)(sum + 0x08));
}

// Send one ATK HUB receiver command and return the 16-byte protocol payload
// without the Windows HID report-ID byte.
static bool ReadAtk8KProtocolCommand(const WCHAR* path, BYTE command,
    WORD eepromAddress, BYTE dataLength, BYTE* outRaw) {
    if (!path || !path[0] || !outRaw) return false;

    HANDLE hRead = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, NULL);
    HANDLE hWrite = CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hRead == INVALID_HANDLE_VALUE || hWrite == INVALID_HANDLE_VALUE) {
        if (hRead != INVALID_HANDLE_VALUE) CloseHandle(hRead);
        if (hWrite != INVALID_HANDLE_VALUE) CloseHandle(hWrite);
        return false;
    }

    HANDLE readEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!readEvent) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    BYTE response[17] = {0};
    OVERLAPPED readOverlapped = {0};
    readOverlapped.hEvent = readEvent;
    DWORD received = 0;
    BOOL readStarted = ReadFile(hRead, response, sizeof(response), &received,
        &readOverlapped);
    DWORD readError = readStarted ? ERROR_SUCCESS : GetLastError();
    if (!readStarted && readError != ERROR_IO_PENDING) {
        CloseHandle(readEvent);
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    BYTE payload[16] = {0};
    payload[0] = command;
    payload[2] = (BYTE)((eepromAddress >> 8) & 0xFF);
    payload[3] = (BYTE)(eepromAddress & 0xFF);
    payload[4] = dataLength;
    payload[15] = Atk8KChecksum(payload);

    BYTE request[17] = {0};
    request[0] = 0x08;
    memcpy(request + 1, payload, sizeof(payload));
    if (!HidD_SetOutputReport(hWrite, request, sizeof(request))) {
        CancelIo(hRead);
        CloseHandle(readEvent);
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    bool success = false;
    ULONGLONG deadline = GetTickCount64() + 900;
    while (GetTickCount64() < deadline) {
        DWORD remaining = (DWORD)(deadline - GetTickCount64());
        if (WaitForSingleObject(readEvent, remaining) != WAIT_OBJECT_0) break;

        received = 0;
        if (GetOverlappedResult(hRead, &readOverlapped, &received, FALSE) &&
            received >= sizeof(response) && response[0] == 0x08 &&
            response[1] == command) {
            memcpy(outRaw, response + 1, 16);
            success = true;
            break;
        }

        ResetEvent(readEvent);
        ZeroMemory(response, sizeof(response));
        received = 0;
        BOOL nextRead = ReadFile(hRead, response, sizeof(response), &received,
            &readOverlapped);
        if (!nextRead && GetLastError() != ERROR_IO_PENDING) break;
    }

    if (!success) CancelIo(hRead);
    CloseHandle(readEvent);
    CloseHandle(hRead);
    CloseHandle(hWrite);
    return success;
}

static bool ReadAtk8KReceiverDpi(const WCHAR* path, int* outLevel,
    int* outDpiX, int* outDpiY) {
    BYTE config[16] = {0};
    if (!ReadAtk8KProtocolCommand(path, 0x08, 0, 10, config)) return false;

    // ATK HUB stores the active DPI index at protocol byte 9. The index is
    // zero-based and selects one of the eight DPI slots.
    int dpiIndex = (int)config[9];
    if (dpiIndex < 0 || dpiIndex > 7) return false;

    static const WORD dpiAddresses[4] = {12, 20, 28, 36};
    BYTE dpiBlock[16] = {0};
    if (!ReadAtk8KProtocolCommand(path, 0x08,
            dpiAddresses[dpiIndex / 2], 8, dpiBlock)) {
        return false;
    }

    int offset = 5 + (dpiIndex % 2) * 4;
    int rawX = (int)dpiBlock[offset];
    int rawY = (int)dpiBlock[offset + 1];
    int dpiEx = (int)dpiBlock[offset + 2];
    // ATK's common PAW3395/3950 encoding uses 50-DPI steps and stores the
    // high two bits for each axis in dpiEx.
    int encodedX = rawX | ((dpiEx & 0x0C) << 6);
    int encodedY = rawY | ((dpiEx & 0xC0) << 2);
    int dpiX = 50 * (encodedX + 1);
    int dpiY = 50 * (encodedY + 1);
    if (dpiX <= 0 || dpiX > 50000 || dpiY <= 0 || dpiY > 50000) return false;

    if (outLevel) *outLevel = dpiIndex + 1;
    if (outDpiX) *outDpiX = dpiX;
    if (outDpiY) *outDpiY = dpiY;
    return true;
}

static bool ReadAtk8KReceiverBattery(const WCHAR* path, int* outBattery) {
    if (!path || !path[0]) return false;

    HANDLE hRead = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, NULL);
    HANDLE hWrite = CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        0, NULL);
    if (hRead == INVALID_HANDLE_VALUE || hWrite == INVALID_HANDLE_VALUE) {
        if (hRead != INVALID_HANDLE_VALUE) CloseHandle(hRead);
        if (hWrite != INVALID_HANDLE_VALUE) CloseHandle(hWrite);
        return false;
    }

    HANDLE readEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!readEvent) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    BYTE response[17] = {0};
    OVERLAPPED readOverlapped = {0};
    readOverlapped.hEvent = readEvent;
    DWORD received = 0;
    BOOL readStarted = ReadFile(hRead, response, sizeof(response), &received,
        &readOverlapped);
    DWORD readError = readStarted ? ERROR_SUCCESS : GetLastError();
    if (!readStarted && readError != ERROR_IO_PENDING) {
        CloseHandle(readEvent);
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    // ATK HUB first sends DownLoadData (command 1), then requests battery
    // level (command 4). Both are report 8 with a 16-byte protocol payload.
    BYTE request[17] = {0};
    request[0] = 0x08;
    request[1] = 0x01;
    request[4] = 0x08;
    DWORD nonce = GetTickCount();
    request[5] = (BYTE)(nonce & 0xFF);
    request[6] = (BYTE)((nonce >> 8) & 0xFF);
    request[7] = (BYTE)((nonce >> 16) & 0xFF);
    request[8] = (BYTE)((nonce >> 24) & 0xFF);
    BYTE checksum = 0;
    for (int i = 0; i < 16; ++i) checksum = (BYTE)(checksum + request[i]);
    request[16] = (BYTE)(0x55 - checksum);
    BOOL discoverySent = HidD_SetOutputReport(hWrite, request, sizeof(request));

    ZeroMemory(request, sizeof(request));
    request[0] = 0x08;
    request[1] = 0x04;
    request[16] = 0x49; // 0x55 - (report id 8 + command 4)
    BOOL sent = HidD_SetOutputReport(hWrite, request, sizeof(request));
    if (!discoverySent || !sent) {
        CancelIo(hRead);
        CloseHandle(readEvent);
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    bool success = false;
    ULONGLONG deadline = GetTickCount64() + 1000;
    while (GetTickCount64() < deadline) {
        DWORD remaining = (DWORD)(deadline - GetTickCount64());
        if (WaitForSingleObject(readEvent, remaining) != WAIT_OBJECT_0) break;

        received = 0;
        if (GetOverlappedResult(hRead, &readOverlapped, &received, FALSE)) {
            if (received >= 7 && response[0] == 0x08 && response[1] == 0x04) {
                int battery = (int)response[6];
                if (battery >= 0 && battery <= 100) {
                    if (outBattery) *outBattery = battery;
                    static volatile LONG lastLoggedReceiverBattery = -1;
                    LONG previous = InterlockedExchange(&lastLoggedReceiverBattery, battery);
                    if (previous != battery) {
                        LogMessage(L"ATK 8K receiver battery response: %d%%", battery);
                    }
                    success = true;
                    break;
                }
            }
        }

        ResetEvent(readEvent);
        ZeroMemory(response, sizeof(response));
        received = 0;
        BOOL nextRead = ReadFile(hRead, response, sizeof(response), &received,
            &readOverlapped);
        if (!nextRead && GetLastError() != ERROR_IO_PENDING) break;
    }

    if (!success) CancelIo(hRead);

    CloseHandle(readEvent);
    CloseHandle(hRead);
    CloseHandle(hWrite);
    return success;
}

static bool ReadAtkBattery(HANDLE hDev, int* outBattery) {
    BYTE feature[8] = {0};
    feature[0] = 0x06; // ATK battery feature report ID
    if (!HidD_GetFeature(hDev, feature, sizeof(feature))) return false;

    // The current ATK report returns the primary battery at byte 3. The
    // following bytes are also battery-like values, but are not used here.
    int battery = (int)feature[3];
    if (battery > 100) return false;
    if (outBattery) *outBattery = battery;
    return true;
}

// N9 headsets expose the ATK HUB serial protocol as a USB COM port.
static bool FindN9ComPort(WCHAR* outPort, DWORD maxLen) {
    if (!outPort || maxLen == 0) return false;
    outPort[0] = L'\0';

    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA data = {0};
    data.cbSize = sizeof(data);
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &data); ++i) {
        WCHAR hardwareIds[2048] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(info, &data, SPDRP_HARDWAREID,
                NULL, (PBYTE)hardwareIds, sizeof(hardwareIds), NULL)) {
            continue;
        }

        WCHAR idsLower[2048] = {0};
        StringCchCopyW(idsLower, ARRAYSIZE(idsLower), hardwareIds);
        _wcslwr_s(idsLower, ARRAYSIZE(idsLower));
        if (!wcsstr(idsLower, L"vid_373b&pid_1204")) continue;

        WCHAR friendly[512] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(info, &data, SPDRP_FRIENDLYNAME,
                NULL, (PBYTE)friendly, sizeof(friendly), NULL)) {
            continue;
        }
        WCHAR* comStart = wcsstr(friendly, L"(COM");
        if (!comStart) comStart = wcsstr(friendly, L"(com");
        if (!comStart) continue;
        WCHAR* comEnd = wcschr(comStart, L')');
        if (!comEnd || comEnd <= comStart + 1) continue;

        WCHAR portName[32] = {0};
        size_t portLength = (size_t)(comEnd - comStart - 1);
        if (portLength >= ARRAYSIZE(portName)) continue;
        StringCchCopyNW(portName, ARRAYSIZE(portName), comStart + 1, portLength);
        StringCchPrintfW(outPort, maxLen, L"\\\\.\\%s", portName);
        found = true;
        break;
    }

    SetupDiDestroyDeviceInfoList(info);
    return found;
}

static bool ReadN9HeadsetBattery(int* outBattery, bool* outCharging) {
    WCHAR portName[64] = {0};
    if (!FindN9ComPort(portName, ARRAYSIZE(portName))) return false;

    HANDLE port = CreateFileW(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    if (port == INVALID_HANDLE_VALUE) return false;

    bool success = false;
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    COMMTIMEOUTS timeouts = {0};
    if (GetCommState(port, &dcb)) {
        dcb.BaudRate = CBR_9600;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        if (SetCommState(port, &dcb)) {
            // Keep a HUB-owned or disconnected COM port from holding the
            // device worker for several seconds during shutdown/reconnect.
            timeouts.ReadIntervalTimeout = 25;
            timeouts.ReadTotalTimeoutConstant = 350;
            timeouts.ReadTotalTimeoutMultiplier = 5;
            timeouts.WriteTotalTimeoutConstant = 350;
            timeouts.WriteTotalTimeoutMultiplier = 5;
            SetCommTimeouts(port, &timeouts);
            PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);

            BYTE request[64] = {0};
            request[0] = 2; // payload length: command + sub-command
            request[1] = 0; // AUTH
            request[2] = 3; // GetBattery
            DWORD written = 0;
            if (WriteFile(port, request, sizeof(request), &written, NULL) && written == sizeof(request)) {
                BYTE response[64] = {0};
                DWORD received = 0;
                if (ReadFile(port, response, sizeof(response), &received, NULL) && received >= 5 &&
                    response[1] == 0 && response[2] == 3) {
                    int raw = (int)response[4];
                    int battery = raw & 0x7f;
                    if (battery <= 100) {
                        if (outBattery) *outBattery = battery;
                        if (outCharging) *outCharging = (raw & 0x80) != 0;
                        success = true;
                    }
                }
            }
        }
    }

    CloseHandle(port);
    return success;
}

static DWORD WINAPI HeadsetWorkerThread(LPVOID lpParam) {
    (void)lpParam;
    LogMessage(L"耳机轮询线程已启动");

    // The headset COM port may be exclusively owned by ATK HUB. Querying it
    // every few seconds is enough for a battery indicator and avoids busy
    // enumeration/open/close cycles that can upset USB serial drivers.
    while (WaitForSingleObject(g_hStopEvent, 500) == WAIT_TIMEOUT) {
        int headsetBattery = 0;
        bool headsetCharging = false;
        bool headsetOk = ReadN9HeadsetBattery(&headsetBattery, &headsetCharging);
        bool headsetWasConnected = InterlockedCompareExchange(&g_headsetConnected, 0, 0) != FALSE;
        int headsetWasBattery = (int)InterlockedCompareExchange(&g_headsetBattery, 0, 0);
        LONG wasCharging = InterlockedCompareExchange(&g_headsetCharging, 0, 0);

        if (headsetOk) {
            if (!headsetWasConnected || headsetWasBattery != headsetBattery ||
                wasCharging != (headsetCharging ? TRUE : FALSE)) {
                UpdateHeadsetState(true, headsetBattery, headsetCharging);
                PostMessageW(g_hMainWnd, WM_APP_HEADSET_UPDATE,
                    (WPARAM)headsetBattery, (LPARAM)(headsetCharging ? 1 : 0));
                LogMessage(L"耳机状态：%d%%，%s", headsetBattery,
                    headsetCharging ? L"充电中" : L"未充电");
            }
        } else if (headsetWasConnected) {
            UpdateHeadsetState(false, -1, false);
            PostMessageW(g_hMainWnd, WM_APP_HEADSET_UPDATE, (WPARAM)-1, 0);
            LogMessage(L"耳机查询失败，等待下次重试（可能被 ATK HUB 占用）");
        }

        if (WaitForSingleObject(g_hStopEvent, 3000) != WAIT_TIMEOUT) break;
    }

    LogMessage(L"耳机轮询线程已退出");
    return 0;
}

#if 0
// Legacy input-report worker retained for reference only.
static DWORD WINAPI HidWorkerThread(LPVOID lpParam) {
    WCHAR devPath[MAX_PATH] = {0};
    WCHAR modelBuf[64] = {0};

    while (WaitForSingleObject(g_hStopEvent, 200) == WAIT_TIMEOUT) {
        if (!FindLegacyNonAtkReportPath(devPath, MAX_PATH, modelBuf, 64)) {
            WCHAR atkPath[MAX_PATH] = {0};
            WCHAR atkModel[64] = {0};
            if (FindAtkBatteryPath(atkPath, MAX_PATH, atkModel, ARRAYSIZE(atkModel))) {
                HANDLE hAtk = CreateFileW(
                    atkPath,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    0,
                    NULL
                );

                if (hAtk != INVALID_HANDLE_VALUE) {
                    StringCchCopyW(g_detectedModel, ARRAYSIZE(g_detectedModel), atkModel);
                    g_deviceConnected = true;
                    PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);

                    int lastAtkBattery = -1;
                    while (WaitForSingleObject(g_hStopEvent, 0) == WAIT_TIMEOUT) {
                        int battery = 0;
                        if (!ReadAtkBattery(hAtk, &battery)) break;

                        InterlockedExchange(&g_battery, battery);
                        if (lastAtkBattery != battery || lastAtkBattery < 0) {
                            PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)battery, 0);
                            lastAtkBattery = battery;
                        }

                        if (WaitForSingleObject(g_hStopEvent, 3000) != WAIT_TIMEOUT) break;
                    }

                    CloseHandle(hAtk);
                    g_deviceConnected = false;
                    PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);
                    continue;
                }
            }

            if (g_deviceConnected) {
                g_deviceConnected = false;
                PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);
            }
            Sleep(1500);
            continue;
        }

        HANDLE hDev = CreateFileW(
            devPath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL
        );

        if (hDev == INVALID_HANDLE_VALUE) {
            if (g_deviceConnected) {
                g_deviceConnected = false;
                PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);
            }
            Sleep(1500);
            continue;
        }

        StringCchCopyW(g_detectedModel, ARRAYSIZE(g_detectedModel), modelBuf);
        g_deviceConnected = true;
        PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);

        HANDLE hReadEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        OVERLAPPED ov = {0};
        ov.hEvent = hReadEvent;

        BYTE buf[65] = {0};
        DWORD bytesRead = 0;
        int lastDpiLevel = -1;
        int lastDpiVal = -1;
        int lastBat = -1;

        while (WaitForSingleObject(g_hStopEvent, 0) == WAIT_TIMEOUT) {
            ResetEvent(hReadEvent);
            BOOL ok = ReadFile(hDev, buf, 65, &bytesRead, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waitHandles[2] = { g_hStopEvent, hReadEvent };
                    DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                    if (waitRes == WAIT_OBJECT_0) {
                        CancelIo(hDev);
                        break;
                    } else if (waitRes == WAIT_OBJECT_0 + 1) {
                        if (!GetOverlappedResult(hDev, &ov, &bytesRead, FALSE)) {
                            break;
                        }
                    } else {
                        CancelIo(hDev);
                        break;
                    }
                } else {
                    break;
                }
            }

            if (bytesRead >= 9 && buf[0] == 0x07) {
                int level = (int)buf[2] + 1;
                int dpix = (int)buf[3] | ((int)buf[4] << 8);
                int dpiy = (int)buf[5] | ((int)buf[6] << 8);
                int status = (int)buf[7];
                int bat = (int)buf[8];

                if (bat > 100) bat = 100;

                bool dpiChanged = (lastDpiLevel != -1 && (level != lastDpiLevel || dpix != lastDpiVal));
                bool batChanged = (lastBat != -1 && bat != lastBat);

                InterlockedExchange(&g_dpiLevel, level);
                InterlockedExchange(&g_dpiX, dpix);
                InterlockedExchange(&g_dpiY, dpiy);
                InterlockedExchange(&g_battery, bat);

                if (dpiChanged) {
                    PostMessageW(g_hMainWnd, WM_APP_DPI_UPDATE, (WPARAM)level, (LPARAM)dpix);
                }
                if (batChanged || lastBat == -1) {
                    PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)bat, 0);
                }

                lastDpiLevel = level;
                lastDpiVal = dpix;
                lastBat = bat;
            }
        }

        g_deviceConnected = false;
        PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);

        CloseHandle(hReadEvent);
        CloseHandle(hDev);
        Sleep(1000);
    }

    return 0;
}

#endif

static DWORD WINAPI HidWorkerThread(LPVOID lpParam) {
    (void)lpParam;
    WCHAR devPath[MAX_PATH] = {0};
    WCHAR modelBuf[64] = {0};

    LogMessage(L"鼠标轮询线程已启动");

    while (WaitForSingleObject(g_hStopEvent, 250) == WAIT_TIMEOUT) {
        WCHAR receiverPath[MAX_PATH] = {0};
        WCHAR receiverModel[64] = {0};
        if (FindAtk8KReceiverPath(receiverPath, ARRAYSIZE(receiverPath),
                receiverModel, ARRAYSIZE(receiverModel))) {
            StringCchCopyW(g_detectedModel, ARRAYSIZE(g_detectedModel), receiverModel);
            g_deviceConnected = true;

            int lastBattery = -1;
            while (WaitForSingleObject(g_hStopEvent, 0) == WAIT_TIMEOUT) {
                int battery = 0;
                if (!ReadAtk8KReceiverBattery(receiverPath, &battery)) break;

                InterlockedExchange(&g_battery, battery);
                if (battery != lastBattery) {
                    lastBattery = battery;
                    PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)battery, 0);
                }

                int dpiLevel = 0;
                int dpiX = 0;
                int dpiY = 0;
                if (ReadAtk8KReceiverDpi(receiverPath, &dpiLevel, &dpiX, &dpiY)) {
                    int oldLevel = (int)InterlockedExchange(&g_dpiLevel, dpiLevel);
                    int oldDpiX = (int)InterlockedExchange(&g_dpiX, dpiX);
                    InterlockedExchange(&g_dpiY, dpiY);
                    if (oldLevel != dpiLevel || oldDpiX != dpiX) {
                        PostMessageW(g_hMainWnd, WM_APP_DPI_UPDATE,
                            (WPARAM)dpiLevel, (LPARAM)dpiX);
                    }
                }

                if (WaitForSingleObject(g_hStopEvent, 3000) != WAIT_TIMEOUT) break;
            }

            g_deviceConnected = false;
            PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);
            continue;
        }

        if (!FindAtkBatteryPath(devPath, ARRAYSIZE(devPath), modelBuf, ARRAYSIZE(modelBuf))) {
            if (g_deviceConnected) {
                g_deviceConnected = false;
                UpdateTrayTooltip();
                PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);
            }
            WaitForSingleObject(g_hStopEvent, 1200);
            continue;
        }

        HANDLE hAtk = CreateFileW(
            devPath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        if (hAtk == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(g_hStopEvent, 1000);
            continue;
        }

        StringCchCopyW(g_detectedModel, ARRAYSIZE(g_detectedModel), modelBuf);
        g_deviceConnected = true;

        int lastBattery = -1;
        while (WaitForSingleObject(g_hStopEvent, 0) == WAIT_TIMEOUT) {
            int battery = 0;
            if (!ReadAtkBattery(hAtk, &battery)) break;

            InterlockedExchange(&g_battery, battery);
            if (battery != lastBattery) {
                lastBattery = battery;
                PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)battery, 0);
            }

            if (WaitForSingleObject(g_hStopEvent, 3000) != WAIT_TIMEOUT) break;
        }

        CloseHandle(hAtk);
        g_deviceConnected = false;
        PostMessageW(g_hMainWnd, WM_APP_BAT_UPDATE, (WPARAM)g_battery, 0);
    }

    LogMessage(L"鼠标轮询线程已退出");
    return 0;
}

static bool IsAutoRunEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH];
        DWORD len = sizeof(path);
        DWORD type = 0;
        LSTATUS st = RegQueryValueExW(hKey, APP_NAME, NULL, &type, (LPBYTE)path, &len);
        RegCloseKey(hKey);
        return (st == ERROR_SUCCESS);
    }
    return false;
}

static void SetAutoRun(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR selfPath[MAX_PATH];
            GetModuleFileNameW(NULL, selfPath, MAX_PATH);
            WCHAR quotedPath[MAX_PATH + 4] = {0};
            StringCchPrintfW(quotedPath, ARRAYSIZE(quotedPath), L"\"%s\"", selfPath);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (const BYTE*)quotedPath,
                (DWORD)((wcslen(quotedPath) + 1) * sizeof(WCHAR)));
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

#if 0
static void LegacyShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    WCHAR bufHeader[64];
    WCHAR bufBat[64];
    WCHAR bufDpi[64];

    if (g_deviceConnected) {
        StringCchPrintfW(bufHeader, 64, L"%s", g_detectedModel);
        StringCchPrintfW(bufBat, 64, L"电池电量: %d%%", g_battery);
        StringCchPrintfW(bufDpi, 64, L"当前 DPI: %d (第 %d 档)", g_dpiX, g_dpiLevel);
    } else {
        StringCchPrintfW(bufHeader, 64, L"ATK Tray（未连接设备）");
        StringCchPrintfW(bufBat, 64, L"电池电量: --");
        StringCchPrintfW(bufDpi, 64, L"当前 DPI: --");
    }

    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_HEADER, bufHeader);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_BATTERY, bufBat);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_DPI, bufDpi);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    UINT autoRunFlags = MF_STRING | (IsAutoRunEnabled() ? MF_CHECKED : MF_UNCHECKED);
    /*
    AppendMenuW(hMenu, autoRunFlags, IDM_AUTORUN, L"开机自启动");
    AppendMenuW(hMenu, MF_STRING, IDM_RECONNECT, L"重新连接外设");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"退出");

    */
    AppendMenuW(hMenu, autoRunFlags, IDM_AUTORUN, L"Autorun");
    AppendMenuW(hMenu, MF_STRING, IDM_RECONNECT, L"Reconnect");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

#endif

static void ShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    WCHAR header[96] = {0};
    WCHAR mouseBattery[64] = {0};
    WCHAR headsetBattery[128] = {0};
    WCHAR dpi[64] = {0};
    if (g_deviceConnected) {
        StringCchCopyW(header, ARRAYSIZE(header), g_detectedModel);
        StringCchPrintfW(mouseBattery, ARRAYSIZE(mouseBattery),
            L"\u9F20\u6807\u7535\u91CF: %d%%", (int)g_battery);
    } else {
        StringCchCopyW(header, ARRAYSIZE(header), L"ATK Tray (\u672A\u8FDE\u63A5\u8BBE\u5907)");
        StringCchCopyW(mouseBattery, ARRAYSIZE(mouseBattery), L"\u9F20\u6807\u7535\u91CF: --");
    }
    if (g_deviceConnected && g_dpiX > 0) {
        StringCchPrintfW(dpi, ARRAYSIZE(dpi),
            L"DPI: %d (\u7b2c %d \u6863)", (int)g_dpiX, (int)g_dpiLevel);
    } else {
        StringCchCopyW(dpi, ARRAYSIZE(dpi), L"DPI: --");
    }
    if (g_headsetConnected && g_headsetBattery >= 0) {
        StringCchPrintfW(headsetBattery, ARRAYSIZE(headsetBattery),
            L"\u8033\u673A\u7535\u91CF: %d%%%s", (int)g_headsetBattery,
            g_headsetCharging ? L" (\u5145\u7535\u4E2D)" : L"");
    } else {
        StringCchCopyW(headsetBattery, ARRAYSIZE(headsetBattery),
            L"\u8033\u673A\u7535\u91CF: -- (\u672A\u8FDE\u63A5\u6216\u88AB ATK HUB \u5360\u7528)");
    }

    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_HEADER, header);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_BATTERY, mouseBattery);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_HEADSET, headsetBattery);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_DPI, dpi);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    UINT autoRunFlags = MF_STRING | (IsAutoRunEnabled() ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, autoRunFlags, IDM_AUTORUN, L"\u5F00\u673A\u81EA\u52A8\u542F\u52A8");
    AppendMenuW(hMenu, MF_STRING, IDM_RECONNECT, L"\u91CD\u65B0\u8FDE\u63A5\u8BBE\u5907");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"\u9000\u51FA ATK Tray");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uTaskbarRestartMsg && g_uTaskbarRestartMsg != 0) {
        UpdateTrayIcon(g_battery);
        return 0;
    }

    switch (msg) {
        case WM_TRAY_ICON: {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowContextMenu(hWnd);
            } else if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                ShowOsdNotification(g_dpiLevel, g_dpiX, g_battery);
            }
            return 0;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDM_AUTORUN: {
                    bool cur = IsAutoRunEnabled();
                    SetAutoRun(!cur);
                    break;
                }
                case IDM_RECONNECT: {
                    SetEvent(g_hStopEvent);
                    if (g_hHidThread) {
                        WaitForSingleObject(g_hHidThread, 5000);
                        CloseHandle(g_hHidThread);
                        g_hHidThread = NULL;
                    }
                    if (g_hHeadsetThread) {
                        WaitForSingleObject(g_hHeadsetThread, 5000);
                        CloseHandle(g_hHeadsetThread);
                        g_hHeadsetThread = NULL;
                    }
                    ResetEvent(g_hStopEvent);
                    g_hHidThread = CreateThread(NULL, 0, HidWorkerThread, NULL, 0, NULL);
                    g_hHeadsetThread = CreateThread(NULL, 0, HeadsetWorkerThread, NULL, 0, NULL);
                    LogMessage(L"用户请求重新连接设备");
                    break;
                }
                case IDM_EXIT: {
                    DestroyWindow(hWnd);
                    break;
                }
            }
            return 0;
        }
        case WM_APP_DPI_UPDATE: {
            int level = (int)wParam;
            int dpix = (int)lParam;
            ShowOsdNotification(level, dpix, g_battery);
            UpdateTrayTooltip();
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            return 0;
        }
        case WM_APP_BAT_UPDATE: {
            int bat = (int)wParam;
            UpdateTrayIcon(bat);
            return 0;
        }
        case WM_APP_HEADSET_UPDATE: {
            UpdateTrayTooltip();
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            return 0;
        }
        case WM_APP_SHOW_STATUS: {
            ShowOsdNotification(g_dpiLevel, g_dpiX, g_battery);
            return 0;
        }
        case WM_SETTINGCHANGE: {
            UpdateTrayIcon(g_battery);
            return 0;
        }
        case WM_DEVICECHANGE: {
            return 0;
        }
        case WM_DESTROY: {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)pCmdLine;
    (void)nCmdShow;
    SetUnhandledExceptionFilter(AtkUnhandledExceptionFilter);
    LogMessage(L"ATK Tray 启动");

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\ATKTraySingleInstance");
    if (!hMutex) {
        LogMessage(L"创建单实例互斥体失败，错误码：%lu", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingWnd = FindWindowW(L"ATKTrayMessageWnd", NULL);
        if (existingWnd) PostMessageW(existingWnd, WM_APP_SHOW_STATUS, 0, 0);
        LogMessage(L"检测到已有实例，当前启动请求退出");
        CloseHandle(hMutex);
        return 0;
    }

    // Enable modern Per-Monitor V2 DPI awareness
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef BOOL (WINAPI *pfnSetDpiAwareV2)(DPI_AWARENESS_CONTEXT);
        pfnSetDpiAwareV2 fnSetDpiAwareV2 = (pfnSetDpiAwareV2)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        if (fnSetDpiAwareV2) {
            fnSetDpiAwareV2(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    g_hInstance = hInstance;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"ATKTrayMessageWnd";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogMessage(L"注册主窗口类失败，错误码：%lu", GetLastError());
        CloseHandle(hMutex);
        return 1;
    }

    g_hMainWnd = CreateWindowExW(0, wc.lpszClassName, L"ATK Tray", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hMainWnd) {
        LogMessage(L"创建主窗口失败，错误码：%lu", GetLastError());
        CloseHandle(hMutex);
        return 1;
    }
    g_uTaskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wcOsd = {0};
    wcOsd.cbSize = sizeof(WNDCLASSEXW);
    wcOsd.lpfnWndProc = OsdWndProc;
    wcOsd.hInstance = hInstance;
    wcOsd.lpszClassName = L"ATKTrayOsdPopupWnd";
    wcOsd.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassExW(&wcOsd) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogMessage(L"注册 OSD 窗口类失败，错误码：%lu", GetLastError());
        DestroyWindow(g_hMainWnd);
        CloseHandle(hMutex);
        return 1;
    }

    g_hOsdWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wcOsd.lpszClassName,
        L"ATK Tray",
        WS_POPUP,
         0, 0, 380, 76,
        NULL, NULL, hInstance, NULL
    );
    if (!g_hOsdWnd) {
        LogMessage(L"创建 OSD 窗口失败，错误码：%lu", GetLastError());
    }

    DEV_BROADCAST_DEVICEINTERFACE_W dbFilter = {0};
    dbFilter.dbcc_size = sizeof(dbFilter);
    dbFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&dbFilter.dbcc_classguid);
    if (!RegisterDeviceNotificationW(g_hMainWnd, &dbFilter, DEVICE_NOTIFY_WINDOW_HANDLE)) {
        LogMessage(L"注册设备通知失败，错误码：%lu", GetLastError());
    }

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_ICON;
    g_nid.hIcon = CreateBatteryIcon(100);
    if (!g_nid.hIcon) {
        LogMessage(L"创建托盘图标失败，错误码：%lu", GetLastError());
    }
    UpdateTrayTooltip();
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        LogMessage(L"添加托盘图标失败，错误码：%lu", GetLastError());
        if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
        if (g_hOsdWnd) DestroyWindow(g_hOsdWnd);
        DestroyWindow(g_hMainWnd);
        CloseHandle(hMutex);
        return 1;
    }

    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_hStopEvent) {
        LogMessage(L"创建停止事件失败，错误码：%lu", GetLastError());
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
        if (g_hOsdWnd) DestroyWindow(g_hOsdWnd);
        DestroyWindow(g_hMainWnd);
        CloseHandle(hMutex);
        return 1;
    }
    g_hHidThread = CreateThread(NULL, 0, HidWorkerThread, NULL, 0, NULL);
    g_hHeadsetThread = CreateThread(NULL, 0, HeadsetWorkerThread, NULL, 0, NULL);
    if (!g_hHidThread || !g_hHeadsetThread) {
        LogMessage(L"创建设备线程失败，错误码：%lu", GetLastError());
        SetEvent(g_hStopEvent);
        if (g_hHidThread) {
            WaitForSingleObject(g_hHidThread, 5000);
            CloseHandle(g_hHidThread);
        }
        if (g_hHeadsetThread) {
            WaitForSingleObject(g_hHeadsetThread, 5000);
            CloseHandle(g_hHeadsetThread);
        }
        CloseHandle(g_hStopEvent);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
        if (g_hOsdWnd) DestroyWindow(g_hOsdWnd);
        DestroyWindow(g_hMainWnd);
        CloseHandle(hMutex);
        return 1;
    }
    LogMessage(L"程序初始化完成");

    MSG msg;
    BOOL messageResult = 0;
    while ((messageResult = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (messageResult < 0) LogMessage(L"消息循环失败，错误码：%lu", GetLastError());

    SetEvent(g_hStopEvent);
    if (g_hHidThread) {
        WaitForSingleObject(g_hHidThread, 5000);
        CloseHandle(g_hHidThread);
    }
    if (g_hHeadsetThread) {
        WaitForSingleObject(g_hHeadsetThread, 5000);
        CloseHandle(g_hHeadsetThread);
    }
    CloseHandle(g_hStopEvent);

    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
    }
    if (g_hOsdWnd) {
        DestroyWindow(g_hOsdWnd);
    }

    CloseHandle(hMutex);
    LogMessage(L"ATK Tray 已退出");
    return (int)msg.wParam;
}
