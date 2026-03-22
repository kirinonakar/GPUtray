#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <deque>
#include <vector>
#include "GpuMonitor.h"

class GraphPopup {
public:
    GraphPopup(HWND hParent, GpuMonitor* monitor);
    ~GraphPopup();

    bool Create();
    void Show(int x, int y);
    void Update(const SystemStats& stats);
    void Hide();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    void OnPaint(HWND hWnd);
    void DrawGraphItem(Gdiplus::Graphics& g, const std::wstring& label, const std::deque<float>& history, int& yPos, Gdiplus::Color color, float currentVal, const std::wstring& unit, const std::wstring& extra = L"");

    HWND m_hWnd;
    HWND m_hParent;
    GpuMonitor* m_monitor;

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
    const int m_height = 700;
};
