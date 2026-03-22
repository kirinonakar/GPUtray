#pragma once

#include <windows.h>
#include <pdh.h>
#include <string>
#include <vector>

struct SystemStats {
    float cpuUsage;
    float cpuTemp;
    float memoryUsage;
    float ramUsed;  // GB
    float ramTotal; // GB
    float gpuUsage;
    float gpuMemoryUsage;
    float gpuMemUsed;  // GB
    float gpuMemTotal; // GB
    float gpuSharedUsed;  // GB
    float gpuSharedTotal; // GB
    float gpuTemp;
};

class GpuMonitor {
public:
    GpuMonitor();
    ~GpuMonitor();

    bool Initialize();
    SystemStats Update();

private:
    // PDH for CPU/GPU Usage
    PDH_HQUERY m_hQuery = nullptr;
    PDH_HCOUNTER m_hCpuCounter = nullptr;
    PDH_HCOUNTER m_hGpuCounter = nullptr;
    std::vector<PDH_HCOUNTER> m_gpuCounters;

    // WMI for Temperatures
    bool InitWmi();
    void CleanupWmi();
    float GetCpuTempWmi();
    float GetGpuTempWmi();

    // GPU Memory via DXGI
    float GetGpuMemoryUsageDxgi();

    bool m_wmiInitialized = false;
};
