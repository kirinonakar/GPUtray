#include "GraphPopup.h"
#include <string>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <cmath>
#include <ctime>
#include <shellapi.h>

using namespace Gdiplus;

#include "TrayIcon.h"

GraphPopup::GraphPopup(HWND hParent, GpuMonitor* monitor, TrayIcon* trayIcon) : m_hWnd(NULL), m_hParent(hParent), m_monitor(monitor), m_trayIcon(trayIcon) {
    LoadSettings();
    UpdateTrayMetrics();
}

GraphPopup::~GraphPopup() {
    if (m_hWnd) DestroyWindow(m_hWnd);
}

bool GraphPopup::Create() {
    WNDCLASSEXW wcex = {sizeof(WNDCLASSEXW)};
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = GraphPopup::WndProc;
    wcex.hInstance = GetModuleHandle(NULL);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = L"GpuTrayPopup";

    RegisterClassExW(&wcex);

    m_hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"GpuTrayPopup", L"", WS_POPUP | WS_BORDER, 0, 0, m_width, m_height, m_hParent, NULL, GetModuleHandle(NULL), this);

    return m_hWnd != NULL;
}

void GraphPopup::Show(int x, int y) {
    POINT anchor = { x, y };
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &monitorInfo);

    int workLeft = static_cast<int>(monitorInfo.rcWork.left);
    int workTop = static_cast<int>(monitorInfo.rcWork.top);
    int workRight = static_cast<int>(monitorInfo.rcWork.right);
    int workBottom = static_cast<int>(monitorInfo.rcWork.bottom);
    int left = std::clamp(x - m_width, workLeft, (std::max)(workLeft, workRight - m_width));
    int top = std::clamp(y - m_height - 10, workTop, (std::max)(workTop, workBottom - m_height));
    SetWindowPos(m_hWnd, HWND_TOPMOST, left, top, m_width, m_height, SWP_SHOWWINDOW);
    SetForegroundWindow(m_hWnd);
}

void GraphPopup::Hide() {
    ShowWindow(m_hWnd, SW_HIDE);
}

void GraphPopup::Update(const SystemStats& stats) {
    m_lastStats = stats;
    if (stats.gpuPowerLimitSupported) {
        if (m_pendingPowerLimitPercent < 70 || m_pendingPowerLimitPercent > 100) {
            m_pendingPowerLimitPercent = stats.gpuPowerLimitPercent;
        }
        if (m_powerLimitDirty && stats.gpuPowerLimitPercent == m_pendingPowerLimitPercent) {
            m_powerLimitDirty = false;
        }
        if (!m_powerLimitDirty) {
            m_pendingPowerLimitPercent = stats.gpuPowerLimitPercent;
        }
    } else {
        m_pendingPowerLimitPercent = -1;
        m_powerLimitDirty = false;
    }
    m_history.cpuUsage.push_back(stats.cpuUsage);
    m_history.memoryUsage.push_back(stats.memoryUsage);
    m_history.gpuUsage.push_back(stats.gpuUsage);
    m_history.gpuMemoryUsage.push_back(stats.gpuMemoryUsage);
    m_history.gpuTemp.push_back(stats.gpuTemp);
    m_history.gpu12VMaxPinCurrent.push_back(stats.gpu12VMaxPinCurrent);

    auto limit = [&](std::deque<float>& h) { if (h.size() > m_historyLimit) h.pop_front(); };
    limit(m_history.cpuUsage); limit(m_history.memoryUsage);
    limit(m_history.gpuUsage); limit(m_history.gpuMemoryUsage); limit(m_history.gpuTemp);
    limit(m_history.gpu12VMaxPinCurrent);

    CheckPinCurrentProtection(stats);

    if (m_hWnd && IsWindowVisible(m_hWnd)) {
        InvalidateRect(m_hWnd, NULL, FALSE);
    }

    if (m_saveLog) {
        std::ofstream log("gputray.csv", std::ios::app);
        if (log.is_open()) {
            // Check if file is empty to write header
            log.seekp(0, std::ios::end);
            if (log.tellp() == 0) {
                log << "\xEF\xBB\xBF\"Timestamp\",CPU Usage(%),RAM Usage(%),GPU Name,GPU Usage(%),GPU Memory(%),GPU Temp(°C),12V Pin1(A),12V Pin2(A),12V Pin3(A),12V Pin4(A),12V Pin5(A),12V Pin6(A),Power Limit(%)" << std::endl;
            }
            
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            struct tm timeinfo;
            localtime_s(&timeinfo, &time_t_now);
            wchar_t timeStr[64];
            wcsftime(timeStr, 64, L"%Y-%m-%d %H:%M:%S", &timeinfo);
            
            std::wstringstream ss;
            ss << L"\"" << timeStr << L"\","
                << (int)stats.cpuUsage << L","
                << (int)stats.memoryUsage << L","
                << L"\"" << stats.gpuName << L"\","
                << (int)stats.gpuUsage << L","
                << (int)stats.gpuMemoryUsage << L","
                << (int)stats.gpuTemp;
            for (float current : stats.gpu12VPinCurrent) ss << L"," << std::fixed << std::setprecision(3) << current;
            ss << L"," << stats.gpuPowerLimitPercent;
            
            std::wstring wideMsg = ss.str();
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideMsg.c_str(), -1, NULL, 0, NULL, NULL);
            if (utf8Len > 0) {
                std::vector<char> utf8Msg(utf8Len);
                WideCharToMultiByte(CP_UTF8, 0, wideMsg.c_str(), -1, utf8Msg.data(), utf8Len, NULL, NULL);
                log << utf8Msg.data() << std::endl;
            }
        }
    }
}

LRESULT CALLBACK GraphPopup::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GraphPopup* pThis = (GraphPopup*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (message) {
    case WM_CREATE:
    {
        CREATESTRUCT* pcs = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pcs->lpCreateParams);
        return 0;
    }
    case WM_PAINT:
        if (pThis) pThis->OnPaint(hWnd);
        return 0;
    case WM_LBUTTONDOWN:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        
        for (const auto& area : pThis->m_clickAreas) {
            if (x >= area.rect.left && x <= area.rect.right && y >= area.rect.top && y <= area.rect.bottom) {
                if (area.action == 0) {
                    bool currentlySelected = pThis->m_selectedMetrics[(int)area.metric];
                    if (!currentlySelected) {
                        // Count total selected
                        int count = 0;
                        for (int i = 0; i < (int)Metric::COUNT; ++i) if (pThis->m_selectedMetrics[i]) count++;
                        if (count >= 5) break; 
                    }
                    pThis->m_selectedMetrics[(int)area.metric] = !currentlySelected;
                    pThis->SaveSettings();
                    pThis->UpdateTrayMetrics();
                    InvalidateRect(hWnd, NULL, FALSE);
                } else if (area.action == 1) {
                    pThis->m_saveLog = !pThis->m_saveLog;
                    pThis->SaveSettings();
                    InvalidateRect(hWnd, NULL, FALSE);
                } else if (area.action == 2) {
                    pThis->AdjustPowerLimit(-5);
                } else if (area.action == 3) {
                    pThis->AdjustPowerLimit(-1);
                } else if (area.action == 4) {
                    pThis->AdjustPowerLimit(1);
                } else if (area.action == 5) {
                    pThis->AdjustPowerLimit(5);
                } else if (area.action == 6) {
                    pThis->AdjustPowerLimit(0, true);
                } else if (area.action == 7) {
                    pThis->ApplyPowerLimit();
                } else if (area.action == 8) {
                    pThis->m_enablePinProtection = !pThis->m_enablePinProtection;
                    pThis->m_pinFaultLatched = false;
                    pThis->SaveSettings();
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                break;
            }
        }

        if (y > pThis->m_height - 70) { // Larger exit button area
            PostQuitMessage(0);
        }
        return 0;
    }
    case WM_KILLFOCUS:
        if (pThis) pThis->Hide();
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void GraphPopup::OnPaint(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    
    Rect clientRect;
    GetClientRect(hWnd, (LPRECT)&clientRect);
    Bitmap memBitmap(clientRect.Width, clientRect.Height);
    Graphics g(&memBitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(255, 30, 30, 30));

    auto formatMem = [](float used, float total) {
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(1) << used << L"/" << total << L"G";
        return ss.str();
    };

    m_clickAreas.clear();

    int y = 10;
    DrawGraphItem(g, Metric::CPU, L"CPU Usage", m_history.cpuUsage, y, Color(255, 100, 200, 255), m_lastStats.cpuUsage, L"%");
    
    std::wstring ramExtra = L" (" + formatMem(m_lastStats.ramUsed, m_lastStats.ramTotal) + L")";
    DrawGraphItem(g, Metric::RAM, L"Memory Usage", m_history.memoryUsage, y, Color(255, 100, 255, 100), m_lastStats.memoryUsage, L"%", ramExtra);
    
    DrawGraphItem(g, Metric::GPU, L"GPU Usage (" + m_lastStats.gpuName + L")", m_history.gpuUsage, y, Color(255, 200, 200, 100), m_lastStats.gpuUsage, L"%");
    
    std::wstring gpuExtra = L" (D:" + formatMem(m_lastStats.gpuMemUsed, m_lastStats.gpuMemTotal) + 
                             L", S:" + formatMem(m_lastStats.gpuSharedUsed, m_lastStats.gpuSharedTotal) + L")";
    DrawGraphItem(g, Metric::GPU_MEM, L"GPU Memory", m_history.gpuMemoryUsage, y, Color(255, 200, 100, 255), m_lastStats.gpuMemoryUsage, L"%", gpuExtra);
    
    DrawGraphItem(g, Metric::GPU_TEMP, L"GPU Temp", m_history.gpuTemp, y,
                  Color(255, 255, 200, 100), m_lastStats.gpuTemp, L"\u00B0C", L"", 100.0f,
                  m_lastStats.gpuTemp > 0.0f);

    std::wstringstream pinExtra;
    if (m_lastStats.gpu12VSupported) {
        pinExtra << std::fixed << std::setprecision(2);
        for (int i = 0; i < 6; ++i) {
            if (i) pinExtra << L" / ";
            pinExtra << L"P" << (i + 1) << L":" << m_lastStats.gpu12VPinCurrent[i] << L"A";
        }
    }
    DrawGraphItem(g, Metric::GPU_12V_CURRENT, L"12V-2x6 Pin Current", m_history.gpu12VMaxPinCurrent, y,
                  Color(255, 255, 100, 180), m_lastStats.gpu12VMaxPinCurrent, L"A", pinExtra.str(), 12.0f,
                  m_lastStats.gpu12VSupported, false, false, false);

    // Power limit control (percentage of the BIOS default TDP).
    Font controlFont(L"Arial", 10, FontStyleRegular);
    SolidBrush controlText(Color(255, 230, 230, 230));
    SolidBrush controlButton(Color(255, 65, 65, 65));
    std::wstringstream powerText;
    powerText << L"GPU Power Limit: ";
    if (m_lastStats.gpuPowerLimitSupported) {
        powerText << m_lastStats.gpuPowerLimitPercent << L"% (" << (int)m_lastStats.gpuPowerLimit << L" W)";
        if (m_powerLimitDirty) {
            float pendingWatts = m_lastStats.gpuPowerLimitDefault * m_pendingPowerLimitPercent / 100.0f;
            powerText << L"  ->  " << m_pendingPowerLimitPercent << L"% (" << (int)std::lround(pendingWatts) << L" W)";
        }
    } else {
        powerText << L"N/A";
    }
    g.DrawString(powerText.str().c_str(), -1, &controlFont, PointF(10, (REAL)y), &controlText);
    if (m_lastStats.gpuPowerLimitSupported) {
        const wchar_t* labels[] = { L"-5%", L"-1%", L"+1%", L"+5%", L"100%", L"Apply" };
        const int widths[] = { 50, 50, 50, 50, 62, 68 };
        int x = 550;
        SolidBrush applyButton(m_powerLimitDirty ? Color(255, 180, 105, 35) : Color(255, 75, 75, 75));
        for (int i = 0; i < 6; ++i) {
            SolidBrush* buttonBrush = i == 5 ? &applyButton : &controlButton;
            g.FillRectangle(buttonBrush, x, y - 3, widths[i], 25);
            g.DrawString(labels[i], -1, &controlFont, PointF((REAL)x + 8, (REAL)y), &controlText);
            ClickArea area = { { x, y - 5, x + widths[i], y + 24 }, Metric::CPU, false, 2 + i };
            m_clickAreas.push_back(area);
            x += widths[i] + 8;
        }
    }
    y += 32;

    // Optional destructive protection is deliberately opt-in.
    Pen protectPen(Color(255, 150, 150, 150), 1.0f);
    Font protectCheckFont(L"Arial", 15, FontStyleBold);
    SolidBrush protectOn(Color(255, 255, 180, 60));
    g.DrawRectangle(&protectPen, 10, y, 20, 20);
    if (m_enablePinProtection) {
        g.DrawString(L"\u2713", -1, &protectCheckFont, PointF(1, (REAL)y - 7), &protectOn);
    }
    g.DrawString(L"Pin safety: warn on 0A / >9.2A and terminate GPU processes >=50%", -1,
                 &controlFont, PointF(40, (REAL)y), &controlText);
    m_clickAreas.push_back({ { 5, y - 4, m_width - 10, y + 24 }, Metric::CPU, false, 8 });

    // Save Log Checkbox
    y += 32;
    Font logFont(L"Arial", 11, FontStyleRegular);
    Font logCheckFont(L"Arial", 15, FontStyleBold);
    SolidBrush logWhiteBrush(Color(255, 230, 230, 230));
    SolidBrush logSelectedBrush(Color(255, 100, 255, 100));

    Pen logCheckPen(Color(255, 150, 150, 150), 1.0f);
    g.DrawRectangle(&logCheckPen, 10, y, 20, 20);
    if (m_saveLog) {
        g.DrawString(L"\u2713", -1, &logCheckFont, PointF(1, (REAL)y - 7), &logSelectedBrush);
    }
    g.DrawString(L"Save metrics to gputray.csv", -1, &logFont, PointF(40, (REAL)y), &logWhiteBrush);
    
    ClickArea logArea;
    logArea.rect = { 5, y - 5, 300, y + 25 };
    logArea.isLog = true;
    logArea.action = 1;
    m_clickAreas.push_back(logArea);

    // Exit Button (centered at bottom, much larger)
    const int btnHeight = 50;
    SolidBrush brush(Color(255, 60, 60, 60));
    g.FillRectangle(&brush, 10, m_height - btnHeight - 10, m_width - 20, btnHeight);
    Font font(L"Arial", 12, FontStyleBold);
    SolidBrush whiteBrush(Color(255, 255, 255, 255));
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(L"Close App", -1, &font, RectF(10, (REAL)m_height - btnHeight - 10, (REAL)m_width - 20, (REAL)btnHeight), &format, &whiteBrush);

    Graphics frontG(hdc);
    frontG.DrawImage(&memBitmap, 0, 0);
    EndPaint(hWnd, &ps);
}

void GraphPopup::DrawGraphItem(Graphics& g, Metric metric, const std::wstring& label, const std::deque<float>& history, int& yPos, Color color, float currentVal, const std::wstring& unit, const std::wstring& extra, float graphMax, bool valueAvailable, bool showCurrentValue, bool showGraph, bool showCheckbox) {
    Font font(L"Arial", 10, FontStyleRegular);
    Font checkFont(L"Arial", 14, FontStyleBold);
    SolidBrush whiteBrush(Color(255, 230, 230, 230));
    SolidBrush selectedBrush(Color(255, 100, 200, 255));
    
    bool isSelected = m_selectedMetrics[(int)metric];
    
    // Checkbox
    const int checkX = 10;
    const int checkY = yPos;
    const int checkSize = 20;
    
    if (showCheckbox) {
        Pen checkPen(Color(255, 150, 150, 150), 1.0f);
        g.DrawRectangle(&checkPen, checkX, checkY, checkSize, checkSize);
        if (isSelected) {
            g.DrawString(L"\u2713", -1, &checkFont, PointF((REAL)checkX-9, (REAL)checkY-7), &selectedBrush);
        }

        ClickArea area;
        area.rect = { checkX - 5, checkY - 5, checkX + checkSize + 200, checkY + checkSize + 5 };
        area.metric = metric;
        area.isLog = false;
        area.action = 0;
        m_clickAreas.push_back(area);
    }

    std::wstring info = label + L": ";
    if (!valueAvailable) {
        info += L"N/A";
    } else if (showCurrentValue) {
        info += std::to_wstring((int)currentVal) + unit + extra;
    } else {
        info += extra;
    }
    g.DrawString(info.c_str(), -1, &font, PointF(showCheckbox ? 40.0f : 10.0f, (REAL)yPos), &whiteBrush);
    
    yPos += 22;
    if (!showGraph) {
        yPos += 15;
        return;
    }
    Pen borderPen(Color(100, 100, 100, 100));
    const int graphHeight = 80;
    const int graphWidth = m_width - 20;
    g.DrawRectangle(&borderPen, 10, yPos, graphWidth, graphHeight);

    if (!history.empty()) {
        Pen linePen(color, 1.5f);
        int x = m_width - 10;
        float prevX = -1, prevY = -1;
        for (auto it = history.rbegin(); it != history.rend() && x >= 10; ++it) {
            float val = *it;
            float h = std::clamp(val, 0.0f, graphMax) * (float)(graphHeight - 2) / graphMax;
            float curY = yPos + (graphHeight - 1) - h;
            if (prevX != -1) {
                g.DrawLine(&linePen, (REAL)x, (REAL)curY, (REAL)prevX, (REAL)prevY);
            }
            prevX = (REAL)x;
            prevY = (REAL)curY;
            x -= 3;
        }
    }
    yPos += graphHeight + 15;
}

void GraphPopup::AdjustPowerLimit(int deltaPercent, bool useDefault) {
    if (!m_lastStats.gpuPowerLimitSupported) return;
    int base = m_pendingPowerLimitPercent >= 70 ? m_pendingPowerLimitPercent : m_lastStats.gpuPowerLimitPercent;
    m_pendingPowerLimitPercent = useDefault ? 100 : std::clamp(base + deltaPercent, 70, 100);
    m_powerLimitDirty = m_pendingPowerLimitPercent != m_lastStats.gpuPowerLimitPercent;
    InvalidateRect(m_hWnd, NULL, FALSE);
}

void GraphPopup::ApplyPowerLimit() {
    if (!m_lastStats.gpuPowerLimitSupported || !m_powerLimitDirty) return;
    int target = m_pendingPowerLimitPercent;

    PowerLimitSetResult result = m_monitor->SetPowerLimitPercent(target);
    if (result == PowerLimitSetResult::Success) {
        InvalidateRect(m_hWnd, NULL, FALSE);
        return;
    }
    if (result == PowerLimitSetResult::RequiresElevation) {
        wchar_t executable[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::wstring parameters = L"--set-power-limit-percent " + std::to_wstring(target);
        HINSTANCE launched = ShellExecuteW(m_hWnd, L"runas", executable, parameters.c_str(), nullptr, SW_HIDE);
        if ((INT_PTR)launched > 32) return;
        MessageBoxW(m_hWnd, L"Administrator approval was cancelled; the power limit was not changed.", L"GPU Power Limit", MB_OK | MB_ICONWARNING);
        return;
    }

    const wchar_t* message = result == PowerLimitSetResult::NotSupported
        ? L"This GPU or driver does not support changing the power limit."
        : result == PowerLimitSetResult::InvalidValue
            ? L"The requested power limit is outside the GPU BIOS range."
            : L"The GPU power limit could not be changed.";
    MessageBoxW(m_hWnd, message, L"GPU Power Limit", MB_OK | MB_ICONERROR);
}

void GraphPopup::CheckPinCurrentProtection(const SystemStats& stats) {
    if (!m_enablePinProtection || !stats.gpu12VSupported) {
        m_pinFaultLatched = false;
        return;
    }

    bool abnormal = false;
    std::wstringstream pins;
    for (int i = 0; i < 6; ++i) {
        float current = stats.gpu12VPinCurrent[i];
        if (current <= 0.01f || current > 9.2f) {
            abnormal = true;
            pins << L"Pin " << (i + 1) << L": " << std::fixed << std::setprecision(2) << current << L" A\n";
        }
    }
    if (!abnormal) {
        m_pinFaultLatched = false;
        return;
    }
    if (m_pinFaultLatched) return;
    m_pinFaultLatched = true;

    std::vector<GpuProcessInfo> processes = m_monitor->GetGpuProcessesAbove(50.0f);
    std::wstringstream warning;
    warning << L"Abnormal 12V-2x6 pin current detected:\n\n" << pins.str();
    if (processes.empty()) {
        warning << L"\nNo process currently exceeds 50% GPU usage.";
    } else {
        warning << L"\nThe following processes will be forcibly terminated after you acknowledge this warning:\n";
        for (const auto& process : processes) {
            warning << L"\n" << process.name << L" (PID " << process.processId << L", " << (int)process.gpuUsage << L"%)";
        }
        warning << L"\n\nUnsaved data in these programs will be lost.";
    }
    MessageBoxW(m_hWnd, warning.str().c_str(), L"GPU 12V-2x6 Current Warning", MB_OK | MB_ICONWARNING | MB_TOPMOST);

    for (const auto& process : processes) {
        HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, process.processId);
        if (handle) {
            TerminateProcess(handle, 0x12);
            CloseHandle(handle);
        }
    }
}

void GraphPopup::LoadSettings() {
    auto readDword = [](const wchar_t* name, DWORD& value) {
        DWORD size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, L"Software\\GpuTray", name,
                            RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
    };

    DWORD value = 0;
    if (readDword(L"PinSafetyEnabled", value)) {
        m_enablePinProtection = value != 0;
    }
    if (readDword(L"SaveLogEnabled", value)) {
        m_saveLog = value != 0;
    }
    if (readDword(L"SelectedMetricsMask", value)) {
        for (int i = 0; i < (int)Metric::COUNT; ++i) {
            m_selectedMetrics[i] = (value & (1u << i)) != 0;
        }
    }
}

void GraphPopup::SaveSettings() const {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\GpuTray", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    DWORD pinEnabled = m_enablePinProtection ? 1u : 0u;
    RegSetValueExW(key, L"PinSafetyEnabled", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&pinEnabled), sizeof(pinEnabled));

    DWORD logEnabled = m_saveLog ? 1u : 0u;
    RegSetValueExW(key, L"SaveLogEnabled", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&logEnabled), sizeof(logEnabled));

    DWORD metricsMask = 0;
    for (int i = 0; i < (int)Metric::COUNT; ++i) {
        if (m_selectedMetrics[i]) metricsMask |= 1u << i;
    }
    RegSetValueExW(key, L"SelectedMetricsMask", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&metricsMask), sizeof(metricsMask));
    RegCloseKey(key);
}

void GraphPopup::UpdateTrayMetrics() {
    std::vector<Metric> active;
    for (int i = 0; i < (int)Metric::COUNT; ++i) {
        if (m_selectedMetrics[i]) {
            active.push_back((Metric)i);
        }
    }
    if (m_trayIcon) {
        m_trayIcon->SetActiveMetrics(active);
    }
}
