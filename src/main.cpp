#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include "GpuMonitor.h"
#include "TrayIcon.h"
#include "GraphPopup.h"
#include "resource.h"

// Globals
GpuMonitor* g_monitor = nullptr;
TrayIcon* g_trayIcon = nullptr;
GraphPopup* g_graphPopup = nullptr;

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_APP + 1: // Tray message
    {
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            g_graphPopup->Show(pt.x, pt.y);
        }
        return 0;
    }
    case WM_TIMER:
    {
        if (wParam == 1) {
            SystemStats stats = g_monitor->Update();
            g_trayIcon->Update(stats);
            g_graphPopup->Update(stats);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Hidden window to handle messages
    const wchar_t CLASS_NAME[] = L"GpuTrayHiddenWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(0, CLASS_NAME, L"GpuTray", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (hWnd == NULL) return 0;

    g_monitor = new GpuMonitor();
    if (!g_monitor->Initialize()) {
        MessageBox(NULL, L"Failed to initialize GPU Monitor", L"Error", MB_ICONERROR);
        return 0;
    }

    g_trayIcon = new TrayIcon(hWnd, g_monitor);
    if (!g_trayIcon->Initialize()) return 0;

    g_graphPopup = new GraphPopup(hWnd, g_monitor);
    if (!g_graphPopup->Create()) return 0;

    // 1s Refresh Timer
    SetTimer(hWnd, 1, 1000, NULL);

    // Message Loop
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    delete g_graphPopup;
    delete g_trayIcon;
    delete g_monitor;

    return 0;
}
