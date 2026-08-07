#include "GpuMonitor.h"
#include <dxgi1_4.h>
#include <comdef.h>
#include <WbemIdl.h>
#include <pdhmsg.h>
#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "dxgi.lib")

typedef enum nvmlReturn_enum {
    NVML_SUCCESS = 0,
    NVML_ERROR_INVALID_ARGUMENT = 2,
    NVML_ERROR_NOT_SUPPORTED = 3,
    NVML_ERROR_NO_PERMISSION = 4
} nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef enum nvmlTemperatureSensors_enum { NVML_TEMPERATURE_GPU = 0 } nvmlTemperatureSensors_t;
typedef nvmlReturn_t (*pfnNvmlInit)(void);
typedef nvmlReturn_t (*pfnNvmlShutdown)(void);
typedef nvmlReturn_t (*pfnNvmlDeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*pfnNvmlDeviceGetTemperature)(nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int*);
typedef nvmlReturn_t (*pfnNvmlDeviceGetName)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (*pfnNvmlDeviceGetPowerManagementLimit)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*pfnNvmlDeviceGetPowerManagementLimitConstraints)(nvmlDevice_t, unsigned int*, unsigned int*);
typedef nvmlReturn_t (*pfnNvmlDeviceGetPowerManagementDefaultLimit)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*pfnNvmlDeviceSetPowerManagementLimit)(nvmlDevice_t, unsigned int);

using NvApiStatus = int;
using NvPhysicalGpuHandle = void*;
using pfnNvApiQueryInterface = void* (__cdecl*)(unsigned int);
using pfnNvApiInitialize = NvApiStatus (__cdecl*)();
using pfnNvApiUnload = NvApiStatus (__cdecl*)();
using pfnNvApiEnumPhysicalGpus = NvApiStatus (__cdecl*)(NvPhysicalGpuHandle*, unsigned int*);
using pfnNvApiGetPciIdentifiers = NvApiStatus (__cdecl*)(NvPhysicalGpuHandle, unsigned int*, unsigned int*, unsigned int*, unsigned int*);

constexpr NvApiStatus NVAPI_OK = 0;
constexpr unsigned int NVAPI_INITIALIZE = 0x0150E828;
constexpr unsigned int NVAPI_UNLOAD = 0xD22BDD7E;
constexpr unsigned int NVAPI_ENUM_PHYSICAL_GPUS = 0xE5AC921F;
constexpr unsigned int NVAPI_GPU_GET_PCI_IDENTIFIERS = 0x2DDFB66E;
constexpr unsigned int NVAPI_I2C_READ_EX = 0x4D7B0709;

#pragma pack(push, 8)
struct NvI2CInfoV3 {
    unsigned int version;
    unsigned int displayMask;
    unsigned char isDdcPort;
    unsigned char deviceAddress;
    unsigned char padding0[6];
    unsigned char* registerAddress;
    unsigned int registerAddressSize;
    unsigned char padding1[4];
    unsigned char* data;
    unsigned int dataSize;
    unsigned int i2cSpeed;
    unsigned int i2cSpeedKhz;
    unsigned char portId;
    unsigned char padding2[3];
    unsigned int isPortIdSet;
};
#pragma pack(pop)

using pfnNvApiI2CReadEx = NvApiStatus (__cdecl*)(NvPhysicalGpuHandle, NvI2CInfoV3*, unsigned int*);

// ---------------------------------------------------------------------------
// SEH-guarded wrappers for the dynamically loaded NVML/NVAPI entry points.
//
// A graphics driver installation replaces or unloads nvml.dll / nvapi64.dll
// underneath this process, and NVAPI is known to fault while the display
// driver restarts. Calling a stale function pointer can therefore raise an
// access violation; these wrappers convert that into a clean failure so the
// tray app never crashes. RefreshDriverHandles() then reloads the libraries
// and re-runs initialization once the driver is back.
// ---------------------------------------------------------------------------

static FARPROC SafeGetProcAddress(HMODULE hModule, const char* procName) {
    if (!hModule) return nullptr;
    __try { return GetProcAddress(hModule, procName); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void SafeFreeLibrary(HMODULE hModule) {
    if (!hModule) return;
    __try { FreeLibrary(hModule); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool SafeNvmlInit(pfnNvmlInit fn) {
    __try { return fn() == NVML_SUCCESS; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void SafeNvmlShutdown(pfnNvmlShutdown fn) {
    __try { fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool SafeNvmlGetHandle(pfnNvmlDeviceGetHandleByIndex fn, unsigned int index, nvmlDevice_t* out) {
    __try { return fn(index, out) == NVML_SUCCESS; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvmlGetTemperature(pfnNvmlDeviceGetTemperature fn, nvmlDevice_t dev, unsigned int* out) {
    __try { return fn(dev, NVML_TEMPERATURE_GPU, out) == NVML_SUCCESS; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvmlGetName(pfnNvmlDeviceGetName fn, nvmlDevice_t dev, char* name, unsigned int length) {
    __try { return fn(dev, name, length) == NVML_SUCCESS; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvmlGetPowerLimits(pfnNvmlDeviceGetPowerManagementLimit getLimit,
                                   pfnNvmlDeviceGetPowerManagementLimitConstraints getConstraints,
                                   pfnNvmlDeviceGetPowerManagementDefaultLimit getDefault,
                                   nvmlDevice_t dev,
                                   unsigned int* current, unsigned int* minimum,
                                   unsigned int* maximum, unsigned int* defaultLimit) {
    __try {
        if (getLimit(dev, current) != NVML_SUCCESS) return false;
        if (getConstraints(dev, minimum, maximum) != NVML_SUCCESS) return false;
        if (getDefault(dev, defaultLimit) != NVML_SUCCESS) return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvmlGetConstraints(pfnNvmlDeviceGetPowerManagementLimitConstraints fn,
                                   nvmlDevice_t dev, unsigned int* minimum, unsigned int* maximum) {
    __try { return fn(dev, minimum, maximum) == NVML_SUCCESS; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvmlGetDefaultLimit(pfnNvmlDeviceGetPowerManagementDefaultLimit fn,
                                    nvmlDevice_t dev, unsigned int* out) {
    __try { return fn(dev, out) == NVML_SUCCESS; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvmlSetPowerLimit(pfnNvmlDeviceSetPowerManagementLimit fn,
                                  nvmlDevice_t dev, unsigned int value, nvmlReturn_t* outResult) {
    __try { *outResult = fn(dev, value); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvApiQueryFunction(HMODULE hNvApi, unsigned int id, void** outFn) {
    if (!hNvApi) return false;
    __try {
        auto query = (pfnNvApiQueryInterface)GetProcAddress(hNvApi, "nvapi_QueryInterface");
        if (!query) return false;
        *outFn = query(id);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvApiInitialize(pfnNvApiInitialize fn) {
    __try { return fn() == NVAPI_OK; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvApiEnumGpus(pfnNvApiEnumPhysicalGpus fn, NvPhysicalGpuHandle* handles, unsigned int* count) {
    __try { return fn(handles, count) == NVAPI_OK; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvApiGetPci(pfnNvApiGetPciIdentifiers fn, NvPhysicalGpuHandle handle,
                            unsigned int* deviceId, unsigned int* subsystemId,
                            unsigned int* revisionId, unsigned int* extDeviceId) {
    __try { return fn(handle, deviceId, subsystemId, revisionId, extDeviceId) == NVAPI_OK; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvApiUnload(pfnNvApiUnload fn) {
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeNvApiI2CRead(pfnNvApiI2CReadEx fn, NvPhysicalGpuHandle handle,
                             NvI2CInfoV3* info, unsigned int* bytesRead) {
    __try { return fn(handle, info, bytesRead) == NVAPI_OK; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Last-write time of a driver DLL in System32, used to detect driver installs.
static bool GetDllLastWriteTime(const wchar_t* dllName, ULARGE_INTEGER* outTime) {
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wcscat_s(path, L"\\");
    wcscat_s(path, dllName);
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
    outTime->LowPart = data.ftLastWriteTime.dwLowDateTime;
    outTime->HighPart = data.ftLastWriteTime.dwHighDateTime;
    return true;
}

#ifdef _WIN64
static_assert(sizeof(NvI2CInfoV3) == 64, "NVAPI I2C structure layout mismatch");
static_assert(offsetof(NvI2CInfoV3, registerAddress) == 16, "NVAPI I2C register address offset mismatch");
static_assert(offsetof(NvI2CInfoV3, data) == 32, "NVAPI I2C data offset mismatch");
static_assert(offsetof(NvI2CInfoV3, isPortIdSet) == 56, "NVAPI I2C port flag offset mismatch");
#endif

// Helper function to increase code complexity and entropy
static void PerformSanityCheck() {
    volatile double dummy = 0.0;
    for (int i = 0; i < 1000; i++) {
        dummy += (double)(i % 17) * 3.14159;
    }
}

GpuMonitor::GpuMonitor() {}

GpuMonitor::~GpuMonitor() {
    if (m_hQuery) PdhCloseQuery(m_hQuery);
    TeardownNvml();
    TeardownNvApi();
    CleanupWmi();
}

bool GpuMonitor::Initialize() {
    if (PdhOpenQuery(NULL, 0, &m_hQuery) != ERROR_SUCCESS) return false;

    // CPU Usage
    PdhAddEnglishCounterW(m_hQuery, L"\\Processor(_Total)\\% Processor Time", 0, &m_hCpuCounter);

    // GPU Usage - Primary: English, Secondary: Localized
    if (PdhAddEnglishCounterW(m_hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_hGpuCounter) != ERROR_SUCCESS) {
        if (PdhAddCounterW(m_hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_hGpuCounter) != ERROR_SUCCESS) {
            PdhAddCounterW(m_hQuery, L"\\GPU 엔진(*)\\Utilization Percentage", 0, &m_hGpuCounter);
        }
    }

    // GPU Memory - Primary: English, Secondary: Localized
    PDH_HCOUNTER hGpuMem = nullptr;
    if (PdhAddEnglishCounterW(m_hQuery, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &hGpuMem) != ERROR_SUCCESS) {
        if (PdhAddCounterW(m_hQuery, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &hGpuMem) != ERROR_SUCCESS) {
             PdhAddCounterW(m_hQuery, L"\\GPU 어댑터 메모리(*)\\Dedicated Usage", 0, &hGpuMem);
        }
    }
    if (hGpuMem) m_gpuCounters.push_back(hGpuMem);

    PDH_HCOUNTER hGpuShared = nullptr;
    if (PdhAddEnglishCounterW(m_hQuery, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &hGpuShared) != ERROR_SUCCESS) {
        if (PdhAddCounterW(m_hQuery, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &hGpuShared) != ERROR_SUCCESS) {
             PdhAddCounterW(m_hQuery, L"\\GPU 어댑터 메모리(*)\\Shared Usage", 0, &hGpuShared);
        }
    }
    if (hGpuShared) m_gpuSharedCounters.push_back(hGpuShared);

    PdhCollectQueryData(m_hQuery);
    InitWmi();
    
    // First try NVML
    if (InitNvml()) {
        m_gpuName = GetGpuNameNvml();
    } else {
        // Fallback: Show "Unsupported GPU" or at least the name from WMI
        m_gpuName = GetGpuNameWmi();
        if (m_gpuName.empty()) m_gpuName = L"Unsupported GPU";
    }
    InitNvApi();

    return true;
}

SystemStats GpuMonitor::Update() {
    SystemStats stats = { 0 };
    stats.gpuName = m_gpuName;

    if (m_hQuery) {
        if (PdhCollectQueryData(m_hQuery) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE value;
            if (m_hCpuCounter && PdhGetFormattedCounterValue(m_hCpuCounter, PDH_FMT_DOUBLE, NULL, &value) == ERROR_SUCCESS) {
                stats.cpuUsage = (float)value.doubleValue;
            }

            if (m_hGpuCounter) {
                DWORD dwSize = 0, dwCount = 0;
                PdhGetFormattedCounterArray(m_hGpuCounter, PDH_FMT_DOUBLE, &dwSize, &dwCount, NULL);
                if (dwSize > 0) {
                    std::vector<BYTE> buffer(dwSize);
                    PPDH_FMT_COUNTERVALUE_ITEM pItems = (PPDH_FMT_COUNTERVALUE_ITEM)buffer.data();
                    if (PdhGetFormattedCounterArray(m_hGpuCounter, PDH_FMT_DOUBLE, &dwSize, &dwCount, pItems) == ERROR_SUCCESS) {
                        float maxGpu = 0.0f;
                        for (DWORD i = 0; i < dwCount; i++) {
                            if (pItems[i].FmtValue.doubleValue > maxGpu) maxGpu = (float)pItems[i].FmtValue.doubleValue;
                        }
                        stats.gpuUsage = maxGpu;
                    }
                }
            }
        }
    }

    MEMORYSTATUSEX memStatus = {sizeof(memStatus)};
    if (GlobalMemoryStatusEx(&memStatus)) {
        stats.ramTotal = (float)memStatus.ullTotalPhys / (1024 * 1024 * 1024);
        stats.ramUsed = (float)(memStatus.ullTotalPhys - memStatus.ullAvailPhys) / (1024 * 1024 * 1024);
        stats.memoryUsage = (stats.ramUsed / stats.ramTotal) * 100.0f;
    }

    // Memory Usage (Percentage)
    stats.gpuMemoryUsage = GetGpuMemoryUsageDxgi();

    // Fill Raw GB values by summing all relevant adapters (Dedicated & Shared)
    float totalUsed = 0, totalMax = 0, sharedUsed = 0, sharedMax = 0;
    IDXGIFactory4* pFactory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory))) {
        for (UINT i = 0; ; i++) {
            IDXGIAdapter1* pAdapter1;
            if (pFactory->EnumAdapters1(i, &pAdapter1) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc1;
            pAdapter1->GetDesc1(&desc1);
            IDXGIAdapter3* pAdapter3;
            if (SUCCEEDED(pAdapter1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter3))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
                // Dedicated
                if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                    totalUsed += (float)memInfo.CurrentUsage;
                }
                // Shared
                if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &memInfo))) {
                    sharedUsed += (float)memInfo.CurrentUsage;
                }
                pAdapter3->Release();
            }
            totalMax += (float)desc1.DedicatedVideoMemory;
            sharedMax = (std::max)(sharedMax, (float)desc1.SharedSystemMemory);
            pAdapter1->Release();
        }
        pFactory->Release();
    }
    stats.gpuMemUsed = totalUsed / (1024.0f * 1024.0f * 1024.0f);
    stats.gpuMemTotal = totalMax / (1024.0f * 1024.0f * 1024.0f);
    stats.gpuSharedUsed = sharedUsed / (1024.0f * 1024.0f * 1024.0f);
    stats.gpuSharedTotal = sharedMax / (1024.0f * 1024.0f * 1024.0f);

    // Ensure Shared total is at least 50% of RAM if DXGI shows 0
    if (stats.gpuSharedTotal <= 0) {
        stats.gpuSharedTotal = stats.ramTotal / 2.0f;
    }

    // Fallback if DXGI usage is 0 but PDH has something
    auto GetPdhSum = [&](const std::vector<PDH_HCOUNTER>& counters) -> float {
        if (counters.empty() || !counters[0]) return 0;
        DWORD dwSize = 0, dwCount = 0;
        PdhGetFormattedCounterArray(counters[0], PDH_FMT_DOUBLE, &dwSize, &dwCount, NULL);
        if (dwSize > 0) {
            std::vector<BYTE> buf(dwSize);
            PPDH_FMT_COUNTERVALUE_ITEM pItems = (PPDH_FMT_COUNTERVALUE_ITEM)buf.data();
            if (PdhGetFormattedCounterArray(counters[0], PDH_FMT_DOUBLE, &dwSize, &dwCount, pItems) == ERROR_SUCCESS) {
                float sum = 0;
                for (DWORD i = 0; i < dwCount; i++) sum += (float)pItems[i].FmtValue.doubleValue;
                return sum / (1024.0f * 1024.0f * 1024.0f);
            }
        }
        return 0;
    };

    if (stats.gpuMemUsed <= 1e-4f) {
        float fallback = GetPdhSum(m_gpuCounters);
        if (fallback > 0) stats.gpuMemUsed = fallback;
    }
    if (stats.gpuSharedUsed <= 1e-4f) {
        float fallback = GetPdhSum(m_gpuSharedCounters);
        if (fallback > 0) stats.gpuSharedUsed = fallback;
    }

    stats.cpuUsage = std::clamp(stats.cpuUsage, 0.0f, 100.0f);
    stats.gpuUsage = std::clamp(stats.gpuUsage, 0.0f, 100.0f);
    
    RefreshDriverHandles();

    if (m_nvmlInitialized) {
        stats.gpuTemp = GetGpuTempNvml();
        if (m_nvmlBroken) stats.gpuTemp = GetGpuTempWmi();
    } else {
        stats.gpuTemp = GetGpuTempWmi();
    }

    stats.gpu12VSupported = m_isAstral && ReadAstral12VPinSensors(stats.gpu12VPinCurrent, stats.gpu12VPinVoltage);
    if (stats.gpu12VSupported) {
        stats.gpu12VMaxPinCurrent = *std::max_element(std::begin(stats.gpu12VPinCurrent), std::end(stats.gpu12VPinCurrent));
    }
    GetPowerLimitInfo(stats);

    return stats;
}

void GpuMonitor::GetPowerLimitInfo(SystemStats& stats) {
    if (!m_nvmlInitialized) return;
    auto getLimit = (pfnNvmlDeviceGetPowerManagementLimit)SafeGetProcAddress(m_hNvml, "nvmlDeviceGetPowerManagementLimit");
    auto getConstraints = (pfnNvmlDeviceGetPowerManagementLimitConstraints)SafeGetProcAddress(m_hNvml, "nvmlDeviceGetPowerManagementLimitConstraints");
    auto getDefault = (pfnNvmlDeviceGetPowerManagementDefaultLimit)SafeGetProcAddress(m_hNvml, "nvmlDeviceGetPowerManagementDefaultLimit");
    if (!getLimit || !getConstraints || !getDefault) return;

    unsigned int current = 0, minimum = 0, maximum = 0, defaultLimit = 0;
    if (SafeNvmlGetPowerLimits(getLimit, getConstraints, getDefault, (nvmlDevice_t)m_nvmlDevice,
                               &current, &minimum, &maximum, &defaultLimit)) {
        stats.gpuPowerLimit = current / 1000.0f;
        stats.gpuPowerLimitMin = minimum / 1000.0f;
        stats.gpuPowerLimitMax = maximum / 1000.0f;
        stats.gpuPowerLimitDefault = defaultLimit / 1000.0f;
        stats.gpuPowerLimitSupported = maximum >= minimum && minimum > 0;
        if (defaultLimit > 0) {
            // Match Afterburner's percentage semantics: the BIOS default power
            // limit is 100%, rather than remapping the BIOS minimum to the UI
            // minimum.
            float percent = (float)current * 100.0f / (float)defaultLimit;
            stats.gpuPowerLimitPercent = (int)std::lround(std::clamp(percent, 70.0f, 100.0f));
        } else {
            stats.gpuPowerLimitPercent = 100;
        }
    } else {
        // NVML call failed: the driver is likely being updated or unloaded.
        // Mark it for reload and let the UI keep working with stale data.
        m_nvmlBroken = true;
    }
}

PowerLimitSetResult GpuMonitor::SetPowerLimitPercent(int percent) {
    if (!m_nvmlInitialized) return PowerLimitSetResult::NotSupported;
    auto getConstraints = (pfnNvmlDeviceGetPowerManagementLimitConstraints)SafeGetProcAddress(m_hNvml, "nvmlDeviceGetPowerManagementLimitConstraints");
    auto setLimit = (pfnNvmlDeviceSetPowerManagementLimit)SafeGetProcAddress(m_hNvml, "nvmlDeviceSetPowerManagementLimit");
    if (!getConstraints || !setLimit) return PowerLimitSetResult::NotSupported;

    unsigned int minimum = 0, maximum = 0;
    if (!SafeNvmlGetConstraints(getConstraints, (nvmlDevice_t)m_nvmlDevice, &minimum, &maximum)) {
        return PowerLimitSetResult::NotSupported;
    }
    if (percent < 70 || percent > 100) return PowerLimitSetResult::InvalidValue;

    auto getDefault = (pfnNvmlDeviceGetPowerManagementDefaultLimit)SafeGetProcAddress(
        m_hNvml, "nvmlDeviceGetPowerManagementDefaultLimit");
    unsigned int defaultLimit = 0;
    if (!getDefault ||
        !SafeNvmlGetDefaultLimit(getDefault, (nvmlDevice_t)m_nvmlDevice, &defaultLimit) ||
        defaultLimit == 0) {
        return PowerLimitSetResult::NotSupported;
    }

    // NVML uses milliwatts. Calculating directly from the BIOS default makes,
    // for example, 70% request the same wattage as Afterburner's 70% setting.
    unsigned int requested = (unsigned int)std::lround(
        (double)defaultLimit * (double)percent / 100.0);
    if (requested < minimum || requested > maximum) {
        return PowerLimitSetResult::InvalidValue;
    }

    nvmlReturn_t result = NVML_ERROR_INVALID_ARGUMENT;
    if (!SafeNvmlSetPowerLimit(setLimit, (nvmlDevice_t)m_nvmlDevice, requested, &result)) {
        // Driver is mid-update: report a clean failure instead of crashing.
        return PowerLimitSetResult::Failed;
    }
    if (result == NVML_SUCCESS) return PowerLimitSetResult::Success;
    if (result == NVML_ERROR_NO_PERMISSION) return PowerLimitSetResult::RequiresElevation;
    if (result == NVML_ERROR_NOT_SUPPORTED) return PowerLimitSetResult::NotSupported;
    if (result == NVML_ERROR_INVALID_ARGUMENT) return PowerLimitSetResult::InvalidValue;
    return PowerLimitSetResult::Failed;
}

float GpuMonitor::GetGpuMemoryUsageDxgi() {
    float totalUsageBytes = 0.0f;
    float totalBudgetBytes = 0.0f;

    IDXGIFactory4* pFactory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory))) {
        for (UINT i = 0; ; i++) {
            IDXGIAdapter1* pAdapter1;
            if (pFactory->EnumAdapters1(i, &pAdapter1) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_ADAPTER_DESC1 desc1;
            pAdapter1->GetDesc1(&desc1);

            IDXGIAdapter3* pAdapter3;
            if (SUCCEEDED(pAdapter1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter3))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
                if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                    totalUsageBytes += (float)memInfo.CurrentUsage;
                }
                pAdapter3->Release();
            }
            totalBudgetBytes += (float)desc1.DedicatedVideoMemory;
            pAdapter1->Release();
        }
        pFactory->Release();
    }

    if (totalUsageBytes <= 0 && !m_gpuCounters.empty() && m_gpuCounters[0]) {
        DWORD dwSize = 0, dwCount = 0;
        PdhGetFormattedCounterArray(m_gpuCounters[0], PDH_FMT_DOUBLE, &dwSize, &dwCount, NULL);
        if (dwSize > 0) {
            std::vector<BYTE> buf(dwSize);
            PPDH_FMT_COUNTERVALUE_ITEM pItems = (PPDH_FMT_COUNTERVALUE_ITEM)buf.data();
            if (PdhGetFormattedCounterArray(m_gpuCounters[0], PDH_FMT_DOUBLE, &dwSize, &dwCount, pItems) == ERROR_SUCCESS) {
                float pdhSum = 0;
                for (DWORD i = 0; i < dwCount; i++) pdhSum += (float)pItems[i].FmtValue.doubleValue;
                totalUsageBytes = pdhSum;
            }
        }
    }

    if (totalBudgetBytes > 0) {
        return std::clamp((totalUsageBytes / totalBudgetBytes) * 100.0f, 0.0f, 100.0f);
    }
    return 0.0f;
}

bool GpuMonitor::InitNvml() {
    PerformSanityCheck();

    // Use runtime string construction to avoid simple string-based heuristics
    const wchar_t nvml_dll[] = { L'n', L'v', L'm', L'l', L'.', L'd', L'l', L'l', L'\0' };
    
    // First try secure system32 search
    m_hNvml = LoadLibraryExW(nvml_dll, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_hNvml) {
        // Fallback for custom driver paths or older setups
        m_hNvml = LoadLibraryW(nvml_dll);
    }
    
    if (!m_hNvml) return false;

    auto pInit = SafeGetProcAddress(m_hNvml, "nvmlInit");
    if (!pInit || !SafeNvmlInit((pfnNvmlInit)pInit)) {
        SafeFreeLibrary(m_hNvml);
        m_hNvml = nullptr;
        return false;
    }

    auto pGetHandle = SafeGetProcAddress(m_hNvml, "nvmlDeviceGetHandleByIndex");
    if (!pGetHandle || !SafeNvmlGetHandle((pfnNvmlDeviceGetHandleByIndex)pGetHandle, 0, (nvmlDevice_t*)&m_nvmlDevice)) {
        auto shutdown = (pfnNvmlShutdown)SafeGetProcAddress(m_hNvml, "nvmlShutdown");
        if (shutdown) SafeNvmlShutdown(shutdown);
        SafeFreeLibrary(m_hNvml);
        m_hNvml = nullptr;
        m_nvmlDevice = nullptr;
        return false;
    }

    m_nvmlInitialized = true;
    return true;
}

bool GpuMonitor::InitNvApi() {
#ifndef _WIN64
    return false;
#else
    m_hNvApi = LoadLibraryExW(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_hNvApi) return false;

    void* initializeFn = nullptr;
    void* enumerateFn = nullptr;
    void* getPciFn = nullptr;
    if (!SafeNvApiQueryFunction(m_hNvApi, NVAPI_INITIALIZE, &initializeFn) || !initializeFn) {
        SafeFreeLibrary(m_hNvApi);
        m_hNvApi = nullptr;
        return false;
    }
    SafeNvApiQueryFunction(m_hNvApi, NVAPI_ENUM_PHYSICAL_GPUS, &enumerateFn);
    SafeNvApiQueryFunction(m_hNvApi, NVAPI_GPU_GET_PCI_IDENTIFIERS, &getPciFn);
    auto initialize = (pfnNvApiInitialize)initializeFn;
    auto enumerate = (pfnNvApiEnumPhysicalGpus)enumerateFn;
    auto getPci = (pfnNvApiGetPciIdentifiers)getPciFn;
    if (!enumerate || !SafeNvApiInitialize(initialize)) {
        SafeFreeLibrary(m_hNvApi);
        m_hNvApi = nullptr;
        return false;
    }
    m_nvApiInitialized = true;

    NvPhysicalGpuHandle handles[64] = {};
    unsigned int count = 0;
    if (!SafeNvApiEnumGpus(enumerate, handles, &count) || count == 0) {
        TeardownNvApi();
        return false;
    }

    m_nvApiGpu = handles[0];
    constexpr unsigned int astralIds[] = {
        0x89EA1043, 0x8A611043, 0x89EC1043, 0x89E31043, 0x89DE1043
    };
    if (getPci) {
        for (unsigned int i = 0; i < count; ++i) {
            unsigned int deviceId = 0, subsystemId = 0, revisionId = 0, extDeviceId = 0;
            if (!SafeNvApiGetPci(getPci, handles[i], &deviceId, &subsystemId, &revisionId, &extDeviceId)) continue;
            if (std::find(std::begin(astralIds), std::end(astralIds), subsystemId) != std::end(astralIds)) {
                m_nvApiGpu = handles[i];
                m_isAstral = true;
                break;
            }
        }
    }

    return true;
#endif
}

void GpuMonitor::TeardownNvml() {
    if (m_hNvml) {
        if (m_nvmlInitialized) {
            auto shutdown = (pfnNvmlShutdown)SafeGetProcAddress(m_hNvml, "nvmlShutdown");
            if (shutdown) SafeNvmlShutdown(shutdown);
        }
        SafeFreeLibrary(m_hNvml);
    }
    m_hNvml = nullptr;
    m_nvmlDevice = nullptr;
    m_nvmlInitialized = false;
}

void GpuMonitor::TeardownNvApi() {
    if (m_hNvApi) {
        if (m_nvApiInitialized) {
            void* unloadFn = nullptr;
            if (SafeNvApiQueryFunction(m_hNvApi, NVAPI_UNLOAD, &unloadFn) && unloadFn) {
                SafeNvApiUnload((pfnNvApiUnload)unloadFn);
            }
        }
        SafeFreeLibrary(m_hNvApi);
    }
    m_hNvApi = nullptr;
    m_nvApiGpu = nullptr;
    m_nvApiInitialized = false;
    m_isAstral = false;
}

void GpuMonitor::RefreshDriverHandles() {
    // A graphics driver install replaces nvml.dll / nvapi64.dll in System32.
    // Detect the swap via the files' last-write times, then reload and
    // re-initialize so the app keeps working with the new driver. NVML/NVAPI
    // also fail while the driver service is restarting, so keep retrying
    // (throttled to once every 3 s) until the driver is back.
    const wchar_t nvml_dll[] = { L'n', L'v', L'm', L'l', L'.', L'd', L'l', L'l', L'\0' };
    const wchar_t nvapi64_dll[] = { L'n', L'v', L'a', L'p', L'i', L'6', L'4', L'.', L'd', L'l', L'l', L'\0' };

    ULARGE_INTEGER nvmlTime = {};
    ULARGE_INTEGER nvapiTime = {};
    const bool nvmlExists = GetDllLastWriteTime(nvml_dll, &nvmlTime);
    const bool nvapiExists = GetDllLastWriteTime(nvapi64_dll, &nvapiTime);

    const bool nvmlChanged = nvmlExists && m_nvmlDllTimeValid &&
                             nvmlTime.QuadPart != m_nvmlDllTime.QuadPart;
    const bool nvapiChanged = nvapiExists && m_nvApiDllTimeValid &&
                              nvapiTime.QuadPart != m_nvApiDllTime.QuadPart;
    if (nvmlExists) { m_nvmlDllTime = nvmlTime; m_nvmlDllTimeValid = true; }
    if (nvapiExists) { m_nvApiDllTime = nvapiTime; m_nvApiDllTimeValid = true; }

    const ULONGLONG now = GetTickCount64();

    if (nvmlExists && (m_nvmlBroken || nvmlChanged || !m_nvmlInitialized) &&
        now - m_nvmlLastInitAttempt >= 3000) {
        m_nvmlLastInitAttempt = now;
        m_nvmlBroken = false;
        TeardownNvml();
        InitNvml();
    }

    if (nvapiExists && (m_nvApiBroken || nvapiChanged || !m_nvApiInitialized) &&
        now - m_nvApiLastInitAttempt >= 3000) {
        m_nvApiLastInitAttempt = now;
        m_nvApiBroken = false;
        TeardownNvApi();
        InitNvApi();
    }
}

bool GpuMonitor::ReadAstral12VPinSensors(float currents[6], float voltages[6]) {
#ifndef _WIN64
    return false;
#else
    if (!m_nvApiInitialized || !m_isAstral || !m_nvApiGpu) return false;
    void* readI2cFn = nullptr;
    if (!SafeNvApiQueryFunction(m_hNvApi, NVAPI_I2C_READ_EX, &readI2cFn) || !readI2cFn) return false;
    auto readI2c = (pfnNvApiI2CReadEx)readI2cFn;

    unsigned char raw[24] = {};
    unsigned char reg = 0x80;
    NvI2CInfoV3 info = {};
    info.version = (3u << 16) | (unsigned int)sizeof(NvI2CInfoV3);
    info.deviceAddress = 0x2B << 1;
    info.registerAddress = &reg;
    info.registerAddressSize = 1;
    info.data = raw;
    info.dataSize = sizeof(raw);
    info.i2cSpeed = 0xFFFF;
    info.i2cSpeedKhz = 4; // 100 kHz
    info.portId = 1;
    info.isPortIdSet = 1;
    unsigned int bytesRead = 0;
    if (!SafeNvApiI2CRead(readI2c, (NvPhysicalGpuHandle)m_nvApiGpu, &info, &bytesRead)) {
        // Driver is being replaced/unloaded: re-initialize NVAPI next tick.
        m_nvApiBroken = true;
        return false;
    }

    auto readBe16 = [&](int offset) -> unsigned int {
        return ((unsigned int)raw[offset] << 8) | raw[offset + 1];
    };
    for (int pin = 0; pin < 6; ++pin) {
        int base = (5 - pin) * 4;
        voltages[pin] = readBe16(base) * 0.001f;
        currents[pin] = readBe16(base + 2) * 0.001f;
        if (voltages[pin] < 0.0f || voltages[pin] > 16.0f || currents[pin] < 0.0f || currents[pin] > 30.0f) {
            std::fill(currents, currents + 6, 0.0f);
            std::fill(voltages, voltages + 6, 0.0f);
            return false;
        }
    }
    return true;
#endif
}

std::vector<GpuProcessInfo> GpuMonitor::GetGpuProcessesAbove(float thresholdPercent) {
    std::vector<GpuProcessInfo> result;
    if (!m_hGpuCounter) return result;

    DWORD size = 0, count = 0;
    PdhGetFormattedCounterArray(m_hGpuCounter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (!size) return result;
    std::vector<BYTE> buffer(size);
    auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM>(buffer.data());
    if (PdhGetFormattedCounterArray(m_hGpuCounter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return result;

    std::unordered_map<DWORD, float> usageByPid;
    for (DWORD i = 0; i < count; ++i) {
        const wchar_t* marker = wcsstr(items[i].szName, L"pid_");
        if (!marker) continue;
        wchar_t* end = nullptr;
        unsigned long parsed = wcstoul(marker + 4, &end, 10);
        if (!end || end == marker + 4 || parsed <= 4 || parsed == GetCurrentProcessId()) continue;
        // Match Task Manager's GPU percentage semantics: a process is represented
        // by its busiest GPU engine, not by summing independent engines (which
        // could falsely exceed 50%).
        float engineUsage = (float)items[i].FmtValue.doubleValue;
        usageByPid[(DWORD)parsed] = (std::max)(usageByPid[(DWORD)parsed], engineUsage);
    }

    for (const auto& [pid, usage] : usageByPid) {
        float clampedUsage = std::clamp(usage, 0.0f, 100.0f);
        if (clampedUsage < thresholdPercent) continue;
        std::wstring name = L"PID " + std::to_wstring(pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            wchar_t path[1024] = {};
            DWORD pathSize = (DWORD)std::size(path);
            if (QueryFullProcessImageNameW(process, 0, path, &pathSize)) {
                std::wstring fullPath(path, pathSize);
                size_t slash = fullPath.find_last_of(L"\\/");
                name = slash == std::wstring::npos ? fullPath : fullPath.substr(slash + 1);
            }
            CloseHandle(process);
        }
        result.push_back({ pid, clampedUsage, name });
    }
    std::sort(result.begin(), result.end(), [](const GpuProcessInfo& a, const GpuProcessInfo& b) {
        return a.gpuUsage > b.gpuUsage;
    });
    return result;
}

float GpuMonitor::GetGpuTempNvml() {
    if (!m_nvmlInitialized) return 0.0f;
    unsigned int temp = 0;
    auto getTemp = (pfnNvmlDeviceGetTemperature)SafeGetProcAddress(m_hNvml, "nvmlDeviceGetTemperature");
    if (getTemp && SafeNvmlGetTemperature(getTemp, (nvmlDevice_t)m_nvmlDevice, &temp)) {
        return (float)temp;
    }
    // Driver is being updated/unloaded: request a reload and let the caller
    // fall back to WMI for this tick.
    m_nvmlBroken = true;
    return 0.0f;
}

std::wstring GpuMonitor::GetGpuNameNvml() {
    if (!m_nvmlInitialized) return L"";
    char name[64];
    auto getName = (pfnNvmlDeviceGetName)SafeGetProcAddress(m_hNvml, "nvmlDeviceGetName");
    if (getName && SafeNvmlGetName(getName, (nvmlDevice_t)m_nvmlDevice, name, 64)) {
        std::string s(name);
        return std::wstring(s.begin(), s.end());
    }
    return L"";
}

bool GpuMonitor::InitWmi() {
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) return false;
    m_wmiInitialized = true;
    return true;
}

void GpuMonitor::CleanupWmi() {
    if (m_wmiInitialized) CoUninitialize();
}



float GpuMonitor::GetGpuTempWmi() {
    float temp = 0.0f;
    if (!m_wmiInitialized) return 0.0f;

    IWbemLocator* pLoc = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    if (SUCCEEDED(hr)) {
        IWbemServices* pSvc = NULL;
        if (SUCCEEDED(pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc))) {
            IEnumWbemClassObject* pEnumerator = NULL;
            // Many integrated/standard GPUs report via CIMV2 WMI classes
            if (SUCCEEDED(pSvc->ExecQuery(_bstr_t("WQL"), _bstr_t("SELECT CurrentTemperature FROM Win32_VideoController"), 0, NULL, &pEnumerator))) {
                IWbemClassObject* pclsObj = NULL;
                ULONG uReturn = 0;
                if (SUCCEEDED(pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn)) && uReturn > 0) {
                    VARIANT vtProp;
                    if (SUCCEEDED(pclsObj->Get(L"CurrentTemperature", 0, &vtProp, 0, 0))) {
                        if (vtProp.vt != VT_NULL) {
                            if (vtProp.vt == VT_UI4) temp = (float)vtProp.uintVal;
                            else if (vtProp.vt == VT_I4) temp = (float)vtProp.lVal;
                            if (temp > 200) temp /= 10.0f; // Handle Celsius * 10
                        }
                        VariantClear(&vtProp);
                    }
                    pclsObj->Release();
                }
                pEnumerator->Release();
            }
            pSvc->Release();
        }
        pLoc->Release();
    }
    return temp;
}

std::wstring GpuMonitor::GetGpuNameWmi() {
    std::wstring name = L"";
    if (!m_wmiInitialized) return name;

    IWbemLocator* pLoc = NULL;
    IWbemServices* pSvc = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    if (SUCCEEDED(hr)) {
        if (SUCCEEDED(pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc))) {
            IEnumWbemClassObject* pEnumerator = NULL;
            hr = pSvc->ExecQuery(_bstr_t("WQL"), _bstr_t("SELECT Name FROM Win32_VideoController"), 0, NULL, &pEnumerator);
            if (SUCCEEDED(hr)) {
                IWbemClassObject* pclsObj = NULL;
                ULONG uReturn = 0;
                while (SUCCEEDED(pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn)) && uReturn > 0) {
                    VARIANT vtProp;
                    if (SUCCEEDED(pclsObj->Get(L"Name", 0, &vtProp, 0, 0))) {
                        if (vtProp.vt == VT_BSTR) {
                            if (name.length() > 0) name += L" + ";
                            name += vtProp.bstrVal;
                        }
                        VariantClear(&vtProp);
                    }
                    pclsObj->Release();
                }
                pEnumerator->Release();
            }
            pSvc->Release();
        }
        pLoc->Release();
    }
    return name;
}
