#pragma once

// Manual update check against this fork's GitHub release, with an in-place install.
//
// Security properties, in order of importance:
//  - Only https://github.com and *.githubusercontent.com are ever contacted, redirects are
//    followed by hand and re-checked against that list, TLS 1.2+ with normal certificate checks.
//  - The release carries build-info.json signed with ECDSA P-256 by CI; the public key is
//    embedded below. The signature covers the commit, its date, the exe size and its SHA-256, so
//    neither the program nor its metadata can be swapped, and an old build cannot be replayed as
//    a newer one.
//  - The downloaded exe must match the signed size and SHA-256 and start with an MZ header
//    before it replaces the running program.
//  - Nothing happens without a button press; no check runs at startup.
// Not covered: a compromise of the signing secret or of the GitHub account that publishes.

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace updater {

// What the running exe knows about itself (recorded at build time, see tools/generate_build_info.cmake)
struct BuildInfo {
    std::wstring commit;      // 40 hex digits, or "unknown" for a build without git
    std::wstring commitDate;  // ISO 8601 with offset, or empty
};
BuildInfo RunningBuild();

// build-info.json from the release, after validation
struct Manifest {
    std::wstring commit;
    std::wstring commitDate;
    uint64_t exeSize = 0;
    std::string exeSha256;           // 64 lowercase hex digits
    std::vector<uint8_t> signature;  // 64 bytes, r || s
};

enum class CheckResult { UpToDate, UpdateAvailable, PublishedIsOlder, Error };

struct CheckOutcome {
    CheckResult result = CheckResult::Error;
    Manifest manifest;
    std::wstring message;  // set when result is Error
};

// Fetches and verifies the release information. Does network I/O: call it off the UI thread.
CheckOutcome CheckForUpdate();

// Downloads the exe named in a verified manifest, checks it, swaps it in over the running
// program and starts the new copy. On success the caller should close; on failure error says why.
bool DownloadAndInstall(const Manifest& manifest, std::wstring& error);

// Deletes the previous program file left behind by an install (best effort). Call at startup.
void RemoveLeftoverOldExe();

// Pieces exposed for tools/updater_tests.cpp
bool ParseUrl(const std::wstring& url, std::wstring& host, std::wstring& path);
bool IsAllowedHost(const std::wstring& host);
bool ResolveRedirect(const std::wstring& currentUrl, const std::wstring& location, std::wstring& next);
bool ParseIso8601(const std::wstring& text, int64_t& unixSeconds);
bool ParseManifest(const std::string& text, Manifest& out, std::wstring& error);
std::string CanonicalSigningText(const Manifest& manifest);
bool VerifyManifestSignature(const Manifest& manifest, const uint8_t* publicKeyBlob, size_t blobSize,
                             std::wstring& error);
CheckResult CompareBuilds(const BuildInfo& running, const Manifest& published);
bool Sha256Hex(const uint8_t* data, size_t size, std::string& hexOut);

}  // namespace updater
