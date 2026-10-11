#include "ProcessEnumerator.h"
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <algorithm>
#include <map>
#include <set>

namespace {

struct RawProcess {
    DWORD pid = 0;
    DWORD parentPid = 0;
    std::wstring name;
    std::wstring path;
    ULONGLONG created = 0;  // FILETIME as a number; 0 when the process could not be opened
};

std::vector<RawProcess> Snapshot() {
    std::vector<RawProcess> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || entry.szExeFile[0] == L'\0') {
                continue;
            }
            RawProcess process;
            process.pid = entry.th32ProcessID;
            process.parentPid = entry.th32ParentProcessID;
            process.name = entry.szExeFile;

            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid);
            if (handle) {
                wchar_t path[MAX_PATH] = {};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(handle, 0, path, &size)) {
                    process.path = path;
                }
                FILETIME created, exited, kernel, user;
                if (GetProcessTimes(handle, &created, &exited, &kernel, &user)) {
                    process.created = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                }
                CloseHandle(handle);
            }
            processes.push_back(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processes;
}

// First visible, unowned, titled top-level window of each process, in Z order
BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM lParam) {
    auto* titles = reinterpret_cast<std::map<DWORD, std::wstring>*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || titles->count(pid)) {
        return TRUE;
    }
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], length + 1);  // a cached caption for other processes, so it cannot hang
    title.resize(static_cast<size_t>(length));
    (*titles)[pid] = title;
    return TRUE;
}

int StateRank(AudioSessionState state) {
    switch (state) {
    case AudioSessionStateActive: return 2;
    case AudioSessionStateInactive: return 1;
    default: return 0;  // expired: the session is over but still listed
    }
}

// Best audio session state per process across every active playback device
void CollectSessions(std::map<DWORD, AudioSessionState>& states) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
        return;
    }
    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT deviceCount = 0;
        devices->GetCount(&deviceCount);
        for (UINT d = 0; d < deviceCount; d++) {
            IMMDevice* device = nullptr;
            if (FAILED(devices->Item(d, &device))) {
                continue;
            }
            IAudioSessionManager2* manager = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&manager)))) {
                IAudioSessionEnumerator* sessions = nullptr;
                if (SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
                    int sessionCount = 0;
                    sessions->GetCount(&sessionCount);
                    for (int s = 0; s < sessionCount; s++) {
                        IAudioSessionControl* control = nullptr;
                        if (FAILED(sessions->GetSession(s, &control))) {
                            continue;
                        }
                        IAudioSessionControl2* control2 = nullptr;
                        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                              reinterpret_cast<void**>(&control2)))) {
                            DWORD pid = 0;
                            AudioSessionState state = AudioSessionStateExpired;
                            if (SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0 && SUCCEEDED(control2->GetState(&state))) {
                                auto existing = states.find(pid);
                                if (existing == states.end() || StateRank(state) > StateRank(existing->second)) {
                                    states[pid] = state;
                                }
                            }
                            control2->Release();
                        }
                        control->Release();
                    }
                    sessions->Release();
                }
                manager->Release();
            }
            device->Release();
        }
        devices->Release();
    }
    enumerator->Release();
}

}  // namespace

ProcessEnumerator::ProcessEnumerator() {
}

ProcessEnumerator::~ProcessEnumerator() {
}

std::vector<ProcessInfo> ProcessEnumerator::GetAllProcesses() {
    std::vector<ProcessInfo> processes;
    for (const RawProcess& raw : Snapshot()) {
        ProcessInfo info;
        info.processId = raw.pid;
        info.parentProcessId = raw.parentPid;
        info.processName = raw.name;
        info.executablePath = raw.path;
        processes.push_back(info);
    }
    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return _wcsicmp(a.processName.c_str(), b.processName.c_str()) < 0;
    });
    return processes;
}

std::vector<ProcessInfo> ProcessEnumerator::GetProcessTrees() {
    std::vector<RawProcess> raw = Snapshot();
    std::map<DWORD, size_t> indexByPid;
    for (size_t i = 0; i < raw.size(); i++) {
        indexByPid[raw[i].pid] = i;
    }

    std::map<DWORD, std::wstring> titles;
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&titles));
    std::map<DWORD, AudioSessionState> sessions;
    CollectSessions(sessions);

    // The root of each process: climb while the parent is alive, has the same name, and started
    // no later than the child. A parent that started later means its PID was reused.
    std::vector<size_t> rootOf(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        size_t current = i;
        std::set<size_t> visited;
        while (visited.insert(current).second) {
            auto parent = indexByPid.find(raw[current].parentPid);
            if (parent == indexByPid.end() || parent->second == current) {
                break;
            }
            const RawProcess& candidate = raw[parent->second];
            if (_wcsicmp(candidate.name.c_str(), raw[current].name.c_str()) != 0) {
                break;
            }
            if (candidate.created != 0 && raw[current].created != 0 && candidate.created > raw[current].created) {
                break;
            }
            current = parent->second;
        }
        rootOf[i] = current;
    }

    const DWORD self = GetCurrentProcessId();
    std::map<size_t, ProcessInfo> rows;  // keyed by root index
    for (size_t i = 0; i < raw.size(); i++) {
        size_t root = rootOf[i];
        if (raw[root].pid == self) {
            continue;  // recording this program's own audio makes no sense
        }
        ProcessInfo& row = rows[root];
        if (row.processId == 0) {
            row.processId = raw[root].pid;
            row.parentProcessId = raw[root].parentPid;
            row.processName = raw[root].name;
            row.executablePath = raw[root].path;
            row.memberCount = 0;
        }
        row.memberCount++;

        auto title = titles.find(raw[i].pid);
        if (title != titles.end()) {
            row.hasWindow = true;
            if (row.windowTitle.empty() || i == root) {
                row.windowTitle = title->second;  // the root's own window describes the app best
            }
        }
        auto session = sessions.find(raw[i].pid);
        if (session != sessions.end()) {
            row.hasAudioSession = true;
            if (session->second == AudioSessionStateActive) {
                row.hasActiveAudio = true;
            }
        }
    }

    std::vector<ProcessInfo> result;
    result.reserve(rows.size());
    for (auto& entry : rows) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        int order = _wcsicmp(a.processName.c_str(), b.processName.c_str());
        return order != 0 ? order < 0 : a.processId < b.processId;
    });
    return result;
}
