// Console harness for the capture path. Opens a process-loopback capture on one PID, records
// for a while, tears everything down, and repeats, reporting whether each take really was
// process-specific. Reproduces upstream issue 15 (a second capture silently becoming
// whole-system loopback) without the GUI, and can write each take to a WAV file so the
// recording can be checked for another program's audio (see tools/check_tones.py).
//
// usage: capture_smoke.exe [pid] [iterations] [seconds] [out-prefix]
//   pid         process to capture; 0 means the whole default playback device; default: this process
//   iterations  how many open/record/close cycles to run; default 3
//   seconds     length of each take; default 1
//   out-prefix  when given, take N is written to <out-prefix>-N.wav with the app's own WavWriter

#include "AudioCapture.h"
#include "WavWriter.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>

int wmain(int argc, wchar_t** argv) {
    DWORD pid = (argc > 1) ? wcstoul(argv[1], nullptr, 10) : GetCurrentProcessId();
    int iterations = (argc > 2) ? _wtoi(argv[2]) : 3;
    int seconds = (argc > 3) ? _wtoi(argv[3]) : 1;
    std::wstring outPrefix = (argc > 4) ? argv[4] : L"";

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // same apartment as the GUI
    if (FAILED(hr)) {
        wprintf(L"CoInitializeEx failed: 0x%08X\n", static_cast<unsigned int>(hr));
        return 2;
    }

    int failures = 0;
    for (int i = 1; i <= iterations; ++i) {
        AudioCapture cap;
        WavWriter writer;
        std::atomic<UINT64> bytes{0};

        bool ok = cap.Initialize(pid);
        bool specific = cap.IsProcessSpecific();

        bool wavOpen = false;
        if (ok && !outPrefix.empty()) {
            std::wstring path = outPrefix + L"-" + std::to_wstring(i) + L".wav";
            wavOpen = writer.Open(path, cap.GetFormat());
            if (!wavOpen) {
                wprintf(L"could not open %s\n", path.c_str());
            }
        }

        cap.SetDataCallback([&](const BYTE* data, UINT32 size) {
            bytes += size;
            if (wavOpen) {
                writer.WriteData(data, size);
            }
        });

        bool started = ok && cap.Start();
        if (started) {
            Sleep(static_cast<DWORD>(seconds) * 1000);
            cap.Stop();
        }
        writer.Close();

        const WAVEFORMATEX* f = cap.GetFormat();
        std::wstring error = cap.GetLastErrorText();
        wprintf(L"take %d: init=%d processSpecific=%d started=%d bytes=%llu format=%u Hz %u ch %u bit%s%s\n",
            i, ok ? 1 : 0, specific ? 1 : 0, started ? 1 : 0, (unsigned long long)bytes.load(),
            f ? f->nSamplesPerSec : 0u, f ? f->nChannels : 0u, f ? f->wBitsPerSample : 0u,
            error.empty() ? L"" : L" error=", error.c_str());

        bool wantedSpecific = (pid != 0);
        if (!ok || specific != wantedSpecific) {
            failures++;
        }
    }

    CoUninitialize();
    wprintf(L"%d of %d takes failed\n", failures, iterations);
    return failures ? 1 : 0;
}
