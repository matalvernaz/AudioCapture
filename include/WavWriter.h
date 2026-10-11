#pragma once

#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <string>
#include <fstream>
#include <vector>
#include <mutex>

class WavWriter {
public:
    WavWriter();
    ~WavWriter();

    // Open WAV file for writing
    bool Open(const std::wstring& filename, const WAVEFORMATEX* format);

    // Write audio data
    bool WriteData(const BYTE* data, UINT32 size);

    // Close file and finalize WAV header
    void Close();

    // Rewrite the RIFF and data sizes with the current values and flush, so a crash leaves a
    // playable file that is at most one flush interval short. Safe to call from another thread.
    void FlushToDisk();

    // Check if file is open
    bool IsOpen() const { return m_file.is_open(); }

private:
    void WriteWavHeader();
    void UpdateWavHeader();
    bool SplitToNextFile();  // Open next file part seamlessly

    static constexpr UINT64 MAX_FILE_SIZE = 4000000000ULL; // ~3.7GB safety limit

    std::mutex m_mutex;              // Serializes Open/WriteData/Close/FlushToDisk
    std::ofstream m_file;
    std::wstring m_filename;
    std::wstring m_baseFilename;     // Base filename without extension
    std::vector<BYTE> m_formatData;  // Store full format (WAVEFORMATEX or WAVEFORMATEXTENSIBLE)
    UINT32 m_dataSize;               // Data size in current file
    UINT64 m_totalDataSize;          // Total data written across all parts
    UINT32 m_filePartNumber;         // Current file part (1, 2, 3...)
    std::streampos m_dataStartPos;
};
