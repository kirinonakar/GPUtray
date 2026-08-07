#pragma once
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <string>
#include <vector>

enum class Metric { CPU, RAM, GPU, GPU_MEM, GPU_TEMP, GPU_12V_CURRENT, COUNT };

enum class PowerLimitSetResult {
    Success,
    RequiresElevation,
    NotSupported,
    InvalidValue,
    Failed
};


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
    float gpu12VPinCurrent[6]; // A, ASUS ROG Astral only
    float gpu12VPinVoltage[6]; // V, ASUS ROG Astral only
    float gpu12VMaxPinCurrent;
    bool gpu12VSupported;
    float gpuPowerLimit;        // W
    float gpuPowerLimitMin;     // W
    float gpuPowerLimitMax;     // W
    float gpuPowerLimitDefault; // W
    int gpuPowerLimitPercent;   // Percentage of the BIOS default TDP (70-100%)
    bool gpuPowerLimitSupported;
    std::wstring gpuName;
};

struct GpuProcessInfo {
    DWORD processId;
    float gpuUsage;
    std::wstring name;
};

class GpuMonitor {
public:
    GpuMonitor();
    ~GpuMonitor();

    bool Initialize();
    SystemStats Update();
    PowerLimitSetResult SetPowerLimitPercent(int percent);
    std::vector<GpuProcessInfo> GetGpuProcessesAbove(float thresholdPercent);

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
    bool m_nvmlBroken = false;
    ULARGE_INTEGER m_nvmlDllTime = {};
    bool m_nvmlDllTimeValid = false;
    ULONGLONG m_nvmlLastInitAttempt = 0;
    void* m_nvmlDevice = nullptr;
    bool InitNvml();
    float GetGpuTempNvml();
    std::wstring GetGpuNameNvml();
    void GetPowerLimitInfo(SystemStats& stats);
    void TeardownNvml();

    // NVAPI for ASUS Astral per-pin current sensors.
    HMODULE m_hNvApi = nullptr;
    bool m_nvApiInitialized = false;
    bool m_nvApiBroken = false;
    ULARGE_INTEGER m_nvApiDllTime = {};
    bool m_nvApiDllTimeValid = false;
    ULONGLONG m_nvApiLastInitAttempt = 0;
    void* m_nvApiGpu = nullptr;
    bool m_isAstral = false;
    bool InitNvApi();
    bool ReadAstral12VPinSensors(float currents[6], float voltages[6]);
    void TeardownNvApi();

    // Driver-update resilience: nvml.dll / nvapi64.dll are replaced while a
    // graphics driver is being installed. Detect the swap and reload both.
    void RefreshDriverHandles();

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
