#include "GraphPopup.h"
#include <string>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <ctime>

using namespace Gdiplus;

#include "TrayIcon.h"

GraphPopup::GraphPopup(HWND hParent, GpuMonitor* monitor, TrayIcon* trayIcon) : m_hWnd(NULL), m_hParent(hParent), m_monitor(monitor), m_trayIcon(trayIcon) {
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
    SetWindowPos(m_hWnd, HWND_TOPMOST, x - m_width, y - m_height - 10, m_width, m_height, SWP_SHOWWINDOW);
    SetForegroundWindow(m_hWnd);
}

void GraphPopup::Hide() {
    ShowWindow(m_hWnd, SW_HIDE);
}

void GraphPopup::Update(const SystemStats& stats) {
    m_lastStats = stats;
    m_history.cpuUsage.push_back(stats.cpuUsage);
    m_history.memoryUsage.push_back(stats.memoryUsage);
    m_history.gpuUsage.push_back(stats.gpuUsage);
    m_history.gpuMemoryUsage.push_back(stats.gpuMemoryUsage);
    m_history.gpuTemp.push_back(stats.gpuTemp);

    auto limit = [&](std::deque<float>& h) { if (h.size() > m_historyLimit) h.pop_front(); };
    limit(m_history.cpuUsage); limit(m_history.memoryUsage);
    limit(m_history.gpuUsage); limit(m_history.gpuMemoryUsage); limit(m_history.gpuTemp);

    if (m_hWnd && IsWindowVisible(m_hWnd)) {
        InvalidateRect(m_hWnd, NULL, FALSE);
    }

    if (m_saveLog) {
        std::ofstream log("gputray.csv", std::ios::app);
        if (log.is_open()) {
            // Check if file is empty to write header
            log.seekp(0, std::ios::end);
            if (log.tellp() == 0) {
                log << "\xEF\xBB\xBF\"Timestamp\",CPU Usage(%),RAM Usage(%),GPU Name,GPU Usage(%),GPU Memory(%),GPU Temp(°C)" << std::endl;
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
                if (!area.isLog) {
                    bool currentlySelected = pThis->m_selectedMetrics[(int)area.metric];
                    if (!currentlySelected) {
                        // Count total selected
                        int count = 0;
                        for (int i = 0; i < (int)Metric::COUNT; ++i) if (pThis->m_selectedMetrics[i]) count++;
                        if (count >= 5) break; 
                    }
                    pThis->m_selectedMetrics[(int)area.metric] = !currentlySelected;
                    pThis->UpdateTrayMetrics();
                    InvalidateRect(hWnd, NULL, FALSE);
                } else {
                    pThis->m_saveLog = !pThis->m_saveLog;
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
    
    std::wstring tempLabel = L"GPU Temp";
    if (m_lastStats.gpuTemp <= 0) {
        tempLabel += L" (N/A)";
    }
    DrawGraphItem(g, Metric::GPU_TEMP, tempLabel, m_history.gpuTemp, y, Color(255, 255, 200, 100), m_lastStats.gpuTemp, L"\u00B0C");

    // Save Log Checkbox
    y += 10;
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

void GraphPopup::DrawGraphItem(Graphics& g, Metric metric, const std::wstring& label, const std::deque<float>& history, int& yPos, Color color, float currentVal, const std::wstring& unit, const std::wstring& extra) {
    Font font(L"Arial", 10, FontStyleRegular);
    Font checkFont(L"Arial", 14, FontStyleBold);
    SolidBrush whiteBrush(Color(255, 230, 230, 230));
    SolidBrush selectedBrush(Color(255, 100, 200, 255));
    
    bool isSelected = m_selectedMetrics[(int)metric];
    
    // Checkbox
    const int checkX = 10;
    const int checkY = yPos;
    const int checkSize = 20;
    
    Pen checkPen(Color(255, 150, 150, 150), 1.0f);
    g.DrawRectangle(&checkPen, checkX, checkY, checkSize, checkSize);
    if (isSelected) {
        g.DrawString(L"\u2713", -1, &checkFont, PointF((REAL)checkX-9, (REAL)checkY-7), &selectedBrush);
    }
    
    ClickArea area;
    area.rect = { checkX - 5, checkY - 5, checkX + checkSize + 200, checkY + checkSize + 5 };
    area.metric = metric;
    area.isLog = false;
    m_clickAreas.push_back(area);

    std::wstring info = label + L": " + std::to_wstring((int)currentVal) + unit + extra;
    g.DrawString(info.c_str(), -1, &font, PointF(40, (REAL)yPos), &whiteBrush);
    
    yPos += 22;
    Pen borderPen(Color(100, 100, 100, 100));
    const int graphHeight = 85;
    const int graphWidth = m_width - 20;
    g.DrawRectangle(&borderPen, 10, yPos, graphWidth, graphHeight);

    if (!history.empty()) {
        Pen linePen(color, 1.5f);
        int x = m_width - 10;
        float prevX = -1, prevY = -1;
        for (auto it = history.rbegin(); it != history.rend() && x >= 10; ++it) {
            float val = *it;
            float h = std::clamp(val, 0.0f, 100.0f) * (float)(graphHeight - 2) / 100.0f;
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
