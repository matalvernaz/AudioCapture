// Prints the rows the process list would show, so the folding and the filters can be checked
// without the window: name, PID, folded member count, window title, audio state.
//
// usage: process_list_check.exe [all]
//   default: what the app shows by default (background processes hidden)
//   all:     every row, with "background" marked

#include "ProcessEnumerator.h"
#include <objbase.h>
#include <cstdio>
#include <cstring>

int wmain(int argc, wchar_t** argv) {
    bool showAll = (argc > 1 && wcscmp(argv[1], L"all") == 0);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 2;
    }

    ProcessEnumerator enumerator;
    std::vector<ProcessInfo> rows = enumerator.GetProcessTrees();
    unsigned int total = 0;
    unsigned int shown = 0;
    unsigned int hidden = 0;
    for (const ProcessInfo& row : rows) {
        total += row.memberCount;
        bool background = !row.hasWindow && !row.hasAudioSession;
        if (background && !showAll) {
            hidden++;
            continue;
        }
        shown++;
        const wchar_t* audio = row.hasActiveAudio ? L"playing" : (row.hasAudioSession ? L"has played" : L"");
        wprintf(L"%-28s %6lu  x%-3u  %-10s %s%s\n", row.processName.c_str(), row.processId, row.memberCount, audio,
                row.windowTitle.c_str(), background ? L"  [background]" : L"");
    }
    wprintf(L"\n%u processes folded into %u rows; %u shown, %u background rows hidden\n",
            total, static_cast<unsigned int>(rows.size()), shown, hidden);

    CoUninitialize();
    return 0;
}
