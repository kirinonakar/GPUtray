#pragma once

#include <windows.h>
#include <string>

enum class StartupTaskResult {
    Success,
    AccessDenied,
    Failed
};

std::wstring GetCurrentExecutablePath();
bool RunElevatedSelfAndWait(HWND parent, const std::wstring& arguments, DWORD* exitCode = nullptr);
StartupTaskResult ConfigureStartupPowerLimitTask(bool enable, const std::wstring& executablePath);
