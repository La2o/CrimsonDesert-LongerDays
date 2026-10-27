#include "toast.h"

#include <atomic>
#include <mutex>

namespace longerdays::toast
{
    namespace
    {
        constexpr UINT      kMsgShow  = WM_APP + 1;
        constexpr UINT_PTR  kTimerId  = 1;
        constexpr ULONGLONG kFadeInMs = 120;
        constexpr ULONGLONG kHoldMs   = 1500; // fully visible, after the fade-in
        constexpr ULONGLONG kFadeOutMs = 400;
        constexpr BYTE      kMaxAlpha = 230;

        constexpr COLORREF kBackground = RGB(22, 20, 18);
        constexpr COLORREF kTitleColor = RGB(235, 230, 220);
        constexpr COLORREF kOnColor    = RGB(255, 186, 84);  // warm sunlight
        constexpr COLORREF kOffColor   = RGB(160, 160, 160);

        // Written by Show (game thread), read by the toast thread.
        std::mutex g_lock;
        HWND       g_over    = nullptr;
        bool       g_enabled = true;

        std::atomic<bool> g_started{false};
        std::atomic<HWND> g_wnd{nullptr};

        // Toast-thread only.
        ULONGLONG g_shownAt = 0;
        HFONT     g_font    = nullptr;
        int       g_fontPx  = 0;
        bool      g_shownEnabled = true;

        const wchar_t* kTitle = L"Longer Days";
        const wchar_t* StateText(bool on) { return on ? L"ON" : L"OFF (vanilla time)"; }

        HFONT FontFor(int px)
        {
            if (g_font && g_fontPx == px) return g_font;
            if (g_font) DeleteObject(g_font);
            g_fontPx = px;
            g_font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            return g_font;
        }

        SIZE Measure(HDC dc, const wchar_t* s)
        {
            RECT r{};
            DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            return {r.right - r.left, r.bottom - r.top};
        }

        // Area of the game we centre on: its client area, or failing that the
        // monitor it is on.
        RECT TargetRect(HWND over)
        {
            RECT r{};
            if (over && IsWindow(over) && !IsIconic(over) && GetClientRect(over, &r) &&
                r.right > r.left && r.bottom > r.top)
            {
                POINT tl{r.left, r.top}, br{r.right, r.bottom};
                ClientToScreen(over, &tl);
                ClientToScreen(over, &br);
                return {tl.x, tl.y, br.x, br.y};
            }
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(over, MONITOR_DEFAULTTOPRIMARY), &mi);
            return mi.rcMonitor;
        }

        // Font and box scale with the game's resolution rather than Windows
        // DPI, so the toast looks the same size at 1080p and 4K.
        int FontPxFor(const RECT& target)
        {
            const int px = (target.bottom - target.top) / 40;
            return px < 14 ? 14 : (px > 60 ? 60 : px);
        }

        void Present(HWND wnd)
        {
            HWND over;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                over = g_over;
                g_shownEnabled = g_enabled;
            }

            const RECT target = TargetRect(over);
            const int  px = FontPxFor(target);

            HDC dc = GetDC(wnd);
            HGDIOBJ old = SelectObject(dc, FontFor(px));
            const SIZE title = Measure(dc, kTitle);
            const SIZE gap   = Measure(dc, L"  ");
            const SIZE state = Measure(dc, StateText(g_shownEnabled));
            SelectObject(dc, old);
            ReleaseDC(wnd, dc);

            const int padX = px, padY = px / 2;
            const int w = padX * 2 + title.cx + gap.cx + state.cx;
            const int h = padY * 2 + title.cy;
            const int x = target.left + ((target.right - target.left) - w) / 2;
            const int y = target.top + (target.bottom - target.top) / 12;

            // The window takes ownership of the region.
            SetWindowRgn(wnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, h / 2, h / 2), FALSE);
            SetLayeredWindowAttributes(wnd, 0, 0, LWA_ALPHA);
            SetWindowPos(wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(wnd, nullptr, TRUE);

            g_shownAt = GetTickCount64();
            SetTimer(wnd, kTimerId, 16, nullptr);
        }

        void Animate(HWND wnd)
        {
            const ULONGLONG t = GetTickCount64() - g_shownAt;
            double a;
            if (t < kFadeInMs)                       a = double(t) / kFadeInMs;
            else if (t < kFadeInMs + kHoldMs)        a = 1.0;
            else if (t < kFadeInMs + kHoldMs + kFadeOutMs)
                a = 1.0 - double(t - kFadeInMs - kHoldMs) / kFadeOutMs;
            else
            {
                KillTimer(wnd, kTimerId);
                ShowWindow(wnd, SW_HIDE);
                return;
            }
            SetLayeredWindowAttributes(wnd, 0, BYTE(a * kMaxAlpha), LWA_ALPHA);
        }

        void Paint(HWND wnd)
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            RECT rc;
            GetClientRect(wnd, &rc);

            HBRUSH bg = CreateSolidBrush(kBackground);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);

            HGDIOBJ old = SelectObject(dc, FontFor(g_fontPx ? g_fontPx : 20));
            SetBkMode(dc, TRANSPARENT);
            const SIZE title = Measure(dc, kTitle);
            const SIZE gap   = Measure(dc, L"  ");

            RECT r = rc;
            r.left += g_fontPx;
            SetTextColor(dc, kTitleColor);
            DrawTextW(dc, kTitle, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            r.left += title.cx + gap.cx;
            SetTextColor(dc, g_shownEnabled ? kOnColor : kOffColor);
            DrawTextW(dc, StateText(g_shownEnabled), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

            SelectObject(dc, old);
            EndPaint(wnd, &ps);
        }

        LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case kMsgShow:         Present(wnd); return 0;
            case WM_TIMER:         if (wp == kTimerId) Animate(wnd); return 0;
            case WM_PAINT:         Paint(wnd); return 0;
            case WM_ERASEBKGND:    return 1;
            case WM_NCHITTEST:     return HTTRANSPARENT;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            }
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        DWORD WINAPI ToastThread(LPVOID)
        {
            // This thread runs window code that lives in our DLL, so it holds
            // a reference to the DLL for as long as it runs: the mod then stays
            // loaded for the rest of the session once a toast has been shown.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&WndProc), &self);

            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc   = WndProc;
            wc.hInstance     = self;
            wc.lpszClassName = L"LongerDaysToast";
            RegisterClassExW(&wc);

            HWND wnd = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                wc.lpszClassName, L"Longer Days", WS_POPUP, 0, 0, 1, 1,
                nullptr, nullptr, self, nullptr);
            if (wnd)
            {
                g_wnd.store(wnd);
                Present(wnd); // the Show() that started this thread

                MSG m;
                while (GetMessageW(&m, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            }
            FreeLibraryAndExitThread(self, 0);
        }
    }

    void Show(HWND over, bool enabled)
    {
        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_over = over;
            g_enabled = enabled;
        }
        // The window gets its own thread with a message loop: the game thread
        // that calls us does not pump messages for our window.
        if (!g_started.exchange(true))
        {
            if (HANDLE t = CreateThread(nullptr, 0, ToastThread, nullptr, 0, nullptr))
                CloseHandle(t);
            return;
        }
        if (HWND wnd = g_wnd.load())
            PostMessageW(wnd, kMsgShow, 0, 0);
    }
}
