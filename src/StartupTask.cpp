#include "StartupTask.h"

#define SECURITY_WIN32
#include <comdef.h>
#include <lmcons.h>
#include <security.h>
#include <secext.h>
#include <shellapi.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kTaskName[] = L"GpuTray Power Limit";
constexpr wchar_t kStartupArguments[] = L"--apply-startup-power-limit";

class ScopedComInitialization {
public:
    ScopedComInitialization() : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedComInitialization() {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
    HRESULT Result() const { return m_result; }

private:
    HRESULT m_result;
};

std::wstring GetCurrentUserName() {
    ULONG extendedSize = 0;
    GetUserNameExW(NameSamCompatible, nullptr, &extendedSize);
    if (GetLastError() == ERROR_MORE_DATA && extendedSize > 0) {
        std::vector<wchar_t> extendedBuffer(extendedSize);
        if (GetUserNameExW(NameSamCompatible, extendedBuffer.data(), &extendedSize)) {
            return std::wstring(extendedBuffer.data());
        }
    }

    DWORD size = 0;
    GetUserNameW(nullptr, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return L"";

    std::vector<wchar_t> buffer(size);
    if (!GetUserNameW(buffer.data(), &size)) return L"";
    return std::wstring(buffer.data());
}

StartupTaskResult ToTaskResult(HRESULT result) {
    if (result == E_ACCESSDENIED || HRESULT_CODE(result) == ERROR_ACCESS_DENIED) {
        return StartupTaskResult::AccessDenied;
    }
    return StartupTaskResult::Failed;
}
}

std::wstring GetCurrentExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return L"";
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

bool RunElevatedSelfAndWait(HWND parent, const std::wstring& arguments, DWORD* exitCode) {
    const std::wstring executable = GetCurrentExecutablePath();
    if (executable.empty()) return false;

    SHELLEXECUTEINFOW executeInfo = { sizeof(executeInfo) };
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    executeInfo.hwnd = parent;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = executable.c_str();
    executeInfo.lpParameters = arguments.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&executeInfo) || !executeInfo.hProcess) return false;

    const DWORD waitResult = WaitForSingleObject(executeInfo.hProcess, INFINITE);
    DWORD helperExitCode = ERROR_GEN_FAILURE;
    const bool gotExitCode = waitResult == WAIT_OBJECT_0 &&
                             GetExitCodeProcess(executeInfo.hProcess, &helperExitCode);
    CloseHandle(executeInfo.hProcess);
    if (exitCode) *exitCode = helperExitCode;
    return gotExitCode;
}

StartupTaskResult ConfigureStartupPowerLimitTask(bool enable, const std::wstring& executablePath) {
    ScopedComInitialization com;
    if (FAILED(com.Result())) return ToTaskResult(com.Result());

    HRESULT result = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                                          RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                          RPC_C_IMP_LEVEL_IMPERSONATE,
                                          nullptr, EOAC_NONE, nullptr);
    if (FAILED(result) && result != RPC_E_TOO_LATE) return ToTaskResult(result);

    ComPtr<ITaskService> service;
    result = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&service));
    if (FAILED(result)) return ToTaskResult(result);

    _variant_t empty;
    result = service->Connect(empty, empty, empty, empty);
    if (FAILED(result)) return ToTaskResult(result);

    ComPtr<ITaskFolder> rootFolder;
    result = service->GetFolder(_bstr_t(L"\\"), &rootFolder);
    if (FAILED(result)) return ToTaskResult(result);

    if (!enable) {
        result = rootFolder->DeleteTask(_bstr_t(kTaskName), 0);
        if (SUCCEEDED(result) || HRESULT_CODE(result) == ERROR_FILE_NOT_FOUND) {
            return StartupTaskResult::Success;
        }
        return ToTaskResult(result);
    }

    if (executablePath.empty()) return StartupTaskResult::Failed;
    const std::wstring userName = GetCurrentUserName();
    if (userName.empty()) return StartupTaskResult::Failed;

    ComPtr<ITaskDefinition> task;
    result = service->NewTask(0, &task);
    if (FAILED(result)) return ToTaskResult(result);

    ComPtr<IRegistrationInfo> registrationInfo;
    result = task->get_RegistrationInfo(&registrationInfo);
    if (FAILED(result)) return ToTaskResult(result);
    if (FAILED(registrationInfo->put_Author(_bstr_t(L"GpuTray Project"))) ||
        FAILED(registrationInfo->put_Description(
            _bstr_t(L"Applies the user-selected NVIDIA GPU power limit when this user signs in.")))) {
        return StartupTaskResult::Failed;
    }

    ComPtr<IPrincipal> principal;
    result = task->get_Principal(&principal);
    if (FAILED(result)) return ToTaskResult(result);
    if (FAILED(principal->put_UserId(_bstr_t(userName.c_str()))) ||
        FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))) {
        return StartupTaskResult::Failed;
    }

    ComPtr<ITaskSettings> settings;
    result = task->get_Settings(&settings);
    if (FAILED(result)) return ToTaskResult(result);
    if (FAILED(settings->put_Enabled(VARIANT_TRUE)) ||
        FAILED(settings->put_StartWhenAvailable(VARIANT_TRUE)) ||
        FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
        FAILED(settings->put_ExecutionTimeLimit(_bstr_t(L"PT5M")))) {
        return StartupTaskResult::Failed;
    }

    ComPtr<ITriggerCollection> triggers;
    result = task->get_Triggers(&triggers);
    if (FAILED(result)) return ToTaskResult(result);

    ComPtr<ITrigger> trigger;
    result = triggers->Create(TASK_TRIGGER_LOGON, &trigger);
    if (FAILED(result)) return ToTaskResult(result);
    ComPtr<ILogonTrigger> logonTrigger;
    result = trigger.As(&logonTrigger);
    if (FAILED(result)) return ToTaskResult(result);
    if (FAILED(logonTrigger->put_Id(_bstr_t(L"GpuTrayUserLogon"))) ||
        FAILED(logonTrigger->put_UserId(_bstr_t(userName.c_str())))) {
        return StartupTaskResult::Failed;
    }

    ComPtr<IActionCollection> actions;
    result = task->get_Actions(&actions);
    if (FAILED(result)) return ToTaskResult(result);

    ComPtr<IAction> action;
    result = actions->Create(TASK_ACTION_EXEC, &action);
    if (FAILED(result)) return ToTaskResult(result);
    ComPtr<IExecAction> execAction;
    result = action.As(&execAction);
    if (FAILED(result)) return ToTaskResult(result);
    if (FAILED(execAction->put_Path(_bstr_t(executablePath.c_str()))) ||
        FAILED(execAction->put_Arguments(_bstr_t(kStartupArguments)))) {
        return StartupTaskResult::Failed;
    }

    ComPtr<IRegisteredTask> registeredTask;
    result = rootFolder->RegisterTaskDefinition(
        _bstr_t(kTaskName), task.Get(), TASK_CREATE_OR_UPDATE,
        _variant_t(userName.c_str()), empty, TASK_LOGON_INTERACTIVE_TOKEN,
        empty, &registeredTask);
    if (FAILED(result)) return ToTaskResult(result);
    return StartupTaskResult::Success;
}
