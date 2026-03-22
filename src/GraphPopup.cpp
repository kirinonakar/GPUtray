#include "GraphPopup.h"
#include <string>
#include <algorithm>
#include <sstream>
#include <iomanip>

using namespace Gdiplus;

GraphPopup::GraphPopup(HWND hParent, GpuMonitor* monitor) : m_hWnd(NULL), m_hParent(hParent), m_monitor(monitor) {
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
        int y = HIWORD(lParam);
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
        ss << L" (" << std::fixed << std::setprecision(1) << used << L" / " << total << L" GB)";
        return ss.str();
    };

    int y = 10;
    DrawGraphItem(g, L"CPU Usage", m_history.cpuUsage, y, Color(255, 100, 200, 255), m_lastStats.cpuUsage, L"%");
    
    std::wstring ramExtra = formatMem(m_lastStats.ramUsed, m_lastStats.ramTotal);
    DrawGraphItem(g, L"Memory Usage", m_history.memoryUsage, y, Color(255, 100, 255, 100), m_lastStats.memoryUsage, L"%", ramExtra);
    
    DrawGraphItem(g, L"GPU Usage (" + m_lastStats.gpuName + L")", m_history.gpuUsage, y, Color(255, 200, 200, 100), m_lastStats.gpuUsage, L"%");
    
    std::wstring gpuExtra = formatMem(m_lastStats.gpuMemUsed, m_lastStats.gpuMemTotal) + 
                             formatMem(m_lastStats.gpuSharedUsed, m_lastStats.gpuSharedTotal);
    DrawGraphItem(g, L"GPU Memory", m_history.gpuMemoryUsage, y, Color(255, 200, 100, 255), m_lastStats.gpuMemoryUsage, L"%", gpuExtra);
    
    std::wstring tempLabel = L"GPU Temp";
    if (m_lastStats.gpuTemp <= 0) {
        tempLabel += L" (N/A)";
    }
    DrawGraphItem(g, tempLabel, m_history.gpuTemp, y, Color(255, 255, 200, 100), m_lastStats.gpuTemp, L"\u00B0C");

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

void GraphPopup::DrawGraphItem(Graphics& g, const std::wstring& label, const std::deque<float>& history, int& yPos, Color color, float currentVal, const std::wstring& unit, const std::wstring& extra) {
    Font font(L"Arial", 10, FontStyleRegular);
    SolidBrush whiteBrush(Color(255, 230, 230, 230));
    
    std::wstring info = label + L": " + std::to_wstring((int)currentVal) + unit + extra;
    g.DrawString(info.c_str(), -1, &font, PointF(10, (REAL)yPos), &whiteBrush);
    
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
