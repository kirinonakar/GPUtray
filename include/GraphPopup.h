#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <deque>
#include <vector>
#include "GpuMonitor.h"

class TrayIcon;
class GraphPopup {
public:
    GraphPopup(HWND hParent, GpuMonitor* monitor, TrayIcon* trayIcon);
    ~GraphPopup();

    bool Create();
    void Show(int x, int y);
    void Update(const SystemStats& stats);
    void Hide();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    void OnPaint(HWND hWnd);
    void DrawGraphItem(Gdiplus::Graphics& g, Metric metric, const std::wstring& label, const std::deque<float>& history, int& yPos, Gdiplus::Color color, float currentVal, const std::wstring& unit, const std::wstring& extra = L"");
    void UpdateTrayMetrics();

    HWND m_hWnd;
    HWND m_hParent;
    GpuMonitor* m_monitor;
    TrayIcon* m_trayIcon;

    bool m_selectedMetrics[(int)Metric::COUNT] = { false, false, true, true, false }; // Default: GPU, GPU_MEM
    bool m_saveLog = false;
    
    struct ClickArea {
        RECT rect;
        Metric metric;
        bool isLog;
    };
    std::vector<ClickArea> m_clickAreas;

    struct HistoryData {
        std::deque<float> cpuUsage;
        std::deque<float> memoryUsage;
        std::deque<float> gpuUsage;
        std::deque<float> gpuMemoryUsage;
        std::deque<float> gpuTemp;
    } m_history;

    SystemStats m_lastStats = { 0 };

    const int m_historyLimit = 100;
    const int m_width = 600;
    const int m_height = 750;
};
