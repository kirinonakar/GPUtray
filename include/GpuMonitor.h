#pragma once
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <string>
#include <vector>

struct SystemStats {
    float cpuUsage;
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
    std::wstring gpuName;
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
    std::vector<PDH_HCOUNTER> m_gpuSharedCounters;

    // NVML for NVIDIA GPUs
    HMODULE m_hNvml = nullptr;
    bool m_nvmlInitialized = false;
    void* m_nvmlDevice = nullptr;
    bool InitNvml();
    float GetGpuTempNvml();
    std::wstring GetGpuNameNvml();

    // WMI for Temperatures and Fallback
    bool InitWmi();
    void CleanupWmi();
    float GetGpuTempWmi();
    std::wstring GetGpuNameWmi();

    // GPU Memory via DXGI
    float GetGpuMemoryUsageDxgi();

    bool m_wmiInitialized = false;
    std::wstring m_gpuName;
};
