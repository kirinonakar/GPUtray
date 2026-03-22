#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <deque>
#include "GpuMonitor.h"

class TrayIcon {
public:
    TrayIcon(HWND hWnd, GpuMonitor* monitor);
    ~TrayIcon();

    bool Initialize();
    void Update(const SystemStats& stats);
    void SetActiveMetrics(const std::vector<Metric>& metrics) { m_activeMetrics = metrics; }
    void ShowContextMenu();

private:
    HICON CreateDynamicIcon(const SystemStats& stats);
    Gdiplus::Color GetColorForUsage(float usage);
    void DrawGraph(Gdiplus::Graphics& g, float value, int yOffset, int height, Gdiplus::Color color);

    HWND m_hWnd;
    GpuMonitor* m_monitor;
    NOTIFYICONDATA m_nid;
    
    ULONG_PTR m_gdiplusToken;
    std::vector<Metric> m_activeMetrics = { Metric::GPU, Metric::GPU_MEM };
};
