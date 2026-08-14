#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <cwchar>
#include "GpuMonitor.h"
#include "TrayIcon.h"
#include "GraphPopup.h"
#include "StartupTask.h"
#include "resource.h"

// Globals
GpuMonitor* g_monitor = nullptr;
TrayIcon* g_trayIcon = nullptr;
GraphPopup* g_graphPopup = nullptr;
UINT g_taskbarCreatedMessage = 0;

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && uMsg == g_taskbarCreatedMessage) {
        if (g_trayIcon) {
            g_trayIcon->RestoreAfterExplorerRestart();
        }
        return 0;
    }

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
    const wchar_t* configureTaskPrefix = L"--configure-startup-task ";
    if (pCmdLine && wcsncmp(pCmdLine, configureTaskPrefix, wcslen(configureTaskPrefix)) == 0) {
        const wchar_t* operation = pCmdLine + wcslen(configureTaskPrefix);
        const bool enable = wcscmp(operation, L"enable") == 0;
        const bool disable = wcscmp(operation, L"disable") == 0;
        if (!enable && !disable) return 1;

        const StartupTaskResult result = ConfigureStartupPowerLimitTask(
            enable, GetCurrentExecutablePath());
        return result == StartupTaskResult::Success ? 0 :
               result == StartupTaskResult::AccessDenied ? 2 : 1;
    }

    const wchar_t* startupPrefix = L"--apply-startup-power-limit";
    if (pCmdLine && wcsncmp(pCmdLine, startupPrefix, wcslen(startupPrefix)) == 0) {
        DWORD percent = 100;
        DWORD size = sizeof(percent);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\GpuTray", L"StartupPowerLimitPercent",
                     RRF_RT_REG_DWORD, nullptr, &percent, &size);
        if (percent < 70 || percent > 100) percent = 100;
        GpuMonitor helper;
        PowerLimitSetResult result = helper.Initialize()
            ? helper.SetPowerLimitPercent((int)percent)
            : PowerLimitSetResult::Failed;
        return result == PowerLimitSetResult::Success ? 0 : 1;
    }

    const wchar_t* setLimitPrefix = L"--set-power-limit-percent ";
    if (pCmdLine && wcsncmp(pCmdLine, setLimitPrefix, wcslen(setLimitPrefix)) == 0) {
        int percent = (int)wcstol(pCmdLine + wcslen(setLimitPrefix), nullptr, 10);
        GpuMonitor helper;
        PowerLimitSetResult result = helper.Initialize()
            ? helper.SetPowerLimitPercent(percent)
            : PowerLimitSetResult::Failed;
        const wchar_t* message = result == PowerLimitSetResult::Success
            ? L"GPU power limit updated successfully."
            : result == PowerLimitSetResult::InvalidValue
                ? L"The requested percentage is outside the 70-100% range or below the GPU BIOS minimum."
                : result == PowerLimitSetResult::NotSupported
                    ? L"This GPU or driver does not support changing the power limit."
                    : L"Failed to update the GPU power limit.";
        MessageBoxW(nullptr, message, L"GPU Power Limit", MB_OK |
                    (result == PowerLimitSetResult::Success ? MB_ICONINFORMATION : MB_ICONERROR));
        return result == PowerLimitSetResult::Success ? 0 : 1;
    }

    // Single instance guard: allow only one tray instance (helpers above may still run).
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\GpuTraySingleInstance");
    if (hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Hidden window to handle messages
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreatedMessage == 0) return 0;

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

    g_graphPopup = new GraphPopup(hWnd, g_monitor, g_trayIcon);
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
    CloseHandle(hMutex);

    return 0;
}
