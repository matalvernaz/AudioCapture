#pragma once

#include <windows.h>
#include <string>
#include <vector>

// One row of the process list. With GetProcessTrees() a row stands for a process together with
// its same-name descendants (a browser and its helper processes, for example), because process
// loopback capture of the parent already records the whole tree.
struct ProcessInfo {
    DWORD processId = 0;
    DWORD parentProcessId = 0;
    std::wstring processName;
    std::wstring executablePath;
    std::wstring windowTitle;      // The root's own window if it has one, else a member's
    unsigned int memberCount = 1;  // This process plus the same-name descendants folded into it
    bool hasWindow = false;        // Some member owns a visible, titled top-level window
    bool hasAudioSession = false;  // Some member has an audio session, playing or not
    bool hasActiveAudio = false;   // Some member is playing right now
};

class ProcessEnumerator {
public:
    ProcessEnumerator();
    ~ProcessEnumerator();

    // One row per application: every process whose parent is not a same-name process, with its
    // same-name descendants folded in. Windows and audio sessions are scanned once each, so this
    // costs about the same as a plain process snapshot. This program's own process is left out.
    std::vector<ProcessInfo> GetProcessTrees();

    // Every process on its own, no folding, no window or audio information
    std::vector<ProcessInfo> GetAllProcesses();
};
