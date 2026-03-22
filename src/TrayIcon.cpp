#include "TrayIcon.h"
#include <shellapi.h>
#include <string>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

TrayIcon::TrayIcon(HWND hWnd, GpuMonitor* monitor) : m_hWnd(hWnd), m_monitor(monitor) {
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL);
}

TrayIcon::~TrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &m_nid);
    GdiplusShutdown(m_gdiplusToken);
}

bool TrayIcon::Initialize() {
    m_nid.cbSize = sizeof(NOTIFYICONDATA);
    m_nid.hWnd = m_hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_APP + 1;
    m_nid.hIcon = CreateDynamicIcon(SystemStats{});
    wcscpy_s(m_nid.szTip, L"GPU Tray Monitor");

    return Shell_NotifyIcon(NIM_ADD, &m_nid);
}

void TrayIcon::Update(const SystemStats& stats) {
    HICON hOldIcon = m_nid.hIcon;
    m_nid.hIcon = CreateDynamicIcon(stats);
    m_nid.uFlags = NIF_ICON;
    Shell_NotifyIcon(NIM_MODIFY, &m_nid);

    if (hOldIcon) DestroyIcon(hOldIcon);
}

HICON TrayIcon::CreateDynamicIcon(const SystemStats& stats) {
    const int size = 16;
    Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(255, 0, 0, 0)); // Black background

    if (!m_activeMetrics.empty()) {
        int barHeight = size / (int)m_activeMetrics.size();
        int y = 0;
        for (const auto& metric : m_activeMetrics) {
            float val = 0;
            switch(metric) {
                case Metric::CPU:      val = stats.cpuUsage; break;
                case Metric::RAM:      val = stats.memoryUsage; break;
                case Metric::GPU:      val = stats.gpuUsage; break;
                case Metric::GPU_MEM:  val = stats.gpuMemoryUsage; break;
                case Metric::GPU_TEMP: val = stats.gpuTemp; break;
            }
            DrawGraph(g, val, y, barHeight, GetColorForUsage(val));
            y += barHeight;
        }
    }

    HICON hIcon;
    bitmap.GetHICON(&hIcon);
    return hIcon;
}

void TrayIcon::DrawGraph(Graphics& g, float value, int yOffset, int height, Color color) {
    if (value <= 0.1f) return; // Completely black for 0 or near 0

    // Background bar for better contrast
    SolidBrush bgBrush(Color(255, 60, 60, 60));
    g.FillRectangle(&bgBrush, 0, yOffset, 16, height);

    int barWidth = (int)(std::clamp(value, 0.0f, 100.0f) * 16.0f / 100.0f);
    if (barWidth < 1 && value > 0.5f) barWidth = 1;

    SolidBrush brush(color);
    g.FillRectangle(&brush, 0, yOffset, barWidth, height);
}

Color TrayIcon::GetColorForUsage(float usage) {
    if (usage < 50.0f) return Color(255, 0, 255, 0); // Green
    if (usage < 80.0f) return Color(255, 255, 255, 0); // Yellow
    return Color(255, 255, 0, 0); // Red
}

void TrayIcon::ShowContextMenu() {
    // This will be called on right-click. 
    // We'll show our custom GraphPopup window here.
}
