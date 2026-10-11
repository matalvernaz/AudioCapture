#include "Updater.h"
#include "SystemDll.h"
#include "build_info.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace updater {
namespace {

// Where releases live. The tag is a rolling "latest", so the asset addresses never change.
const wchar_t* const kManifestUrl =
    L"https://github.com/matalvernaz/AudioCapture/releases/download/latest/build-info.json";
const wchar_t* const kExeUrl =
    L"https://github.com/matalvernaz/AudioCapture/releases/download/latest/AudioCapture.exe";

// BCRYPT_ECCPUBLIC_BLOB (magic "ECS1", 32-byte key, X, Y) of the key whose private half signs
// build-info.json in CI. Rotating the key means a new blob here and one manual update.
const uint8_t kPublicKeyBlob[72] = {
    0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00,
    0xa3, 0x3f, 0x57, 0x9b, 0x92, 0xbd, 0xa8, 0xf4, 0xb0, 0xde, 0xab, 0x6c, 0x08, 0x2b, 0xae, 0xd3,
    0x0e, 0x64, 0x7a, 0xc8, 0xbc, 0x4d, 0xfa, 0x37, 0x4c, 0x12, 0xac, 0x16, 0x87, 0x0f, 0x70, 0xc2,
    0x1f, 0xc2, 0xb1, 0x38, 0x5a, 0xde, 0x5b, 0xc6, 0xfd, 0x34, 0xab, 0xa7, 0x66, 0x1f, 0x00, 0x44,
    0x25, 0x91, 0xf2, 0x3f, 0xed, 0xe8, 0x35, 0xc9, 0x63, 0xf8, 0xe6, 0x24, 0xa9, 0xa0, 0x06, 0xf1,
};

const size_t kMaxManifestBytes = 64 * 1024;
const uint64_t kMinExeBytes = 100 * 1024;
const uint64_t kMaxExeBytes = 20 * 1024 * 1024;
const int kMaxRedirects = 5;

//=============================================================================
// System DLLs, loaded from System32 only and resolved by name
//=============================================================================

struct WinHttpApi {
    bool ok = false;
    decltype(&::WinHttpOpen) Open = nullptr;
    decltype(&::WinHttpSetOption) SetOption = nullptr;
    decltype(&::WinHttpSetTimeouts) SetTimeouts = nullptr;
    decltype(&::WinHttpConnect) Connect = nullptr;
    decltype(&::WinHttpOpenRequest) OpenRequest = nullptr;
    decltype(&::WinHttpSendRequest) SendRequest = nullptr;
    decltype(&::WinHttpReceiveResponse) ReceiveResponse = nullptr;
    decltype(&::WinHttpQueryHeaders) QueryHeaders = nullptr;
    decltype(&::WinHttpReadData) ReadData = nullptr;
    decltype(&::WinHttpCloseHandle) Close = nullptr;

    WinHttpApi() {
        HMODULE module = LoadSystemDll(L"winhttp.dll");
        if (!module) {
            return;
        }
#define RESOLVE(member, name) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name)); \
        if (!member) return;
        RESOLVE(Open, "WinHttpOpen")
        RESOLVE(SetOption, "WinHttpSetOption")
        RESOLVE(SetTimeouts, "WinHttpSetTimeouts")
        RESOLVE(Connect, "WinHttpConnect")
        RESOLVE(OpenRequest, "WinHttpOpenRequest")
        RESOLVE(SendRequest, "WinHttpSendRequest")
        RESOLVE(ReceiveResponse, "WinHttpReceiveResponse")
        RESOLVE(QueryHeaders, "WinHttpQueryHeaders")
        RESOLVE(ReadData, "WinHttpReadData")
        RESOLVE(Close, "WinHttpCloseHandle")
#undef RESOLVE
        ok = true;
    }
};

struct BCryptApi {
    bool ok = false;
    decltype(&::BCryptOpenAlgorithmProvider) OpenAlgorithmProvider = nullptr;
    decltype(&::BCryptCloseAlgorithmProvider) CloseAlgorithmProvider = nullptr;
    decltype(&::BCryptCreateHash) CreateHash = nullptr;
    decltype(&::BCryptHashData) HashData = nullptr;
    decltype(&::BCryptFinishHash) FinishHash = nullptr;
    decltype(&::BCryptDestroyHash) DestroyHash = nullptr;
    decltype(&::BCryptImportKeyPair) ImportKeyPair = nullptr;
    decltype(&::BCryptVerifySignature) VerifySignature = nullptr;
    decltype(&::BCryptDestroyKey) DestroyKey = nullptr;

    BCryptApi() {
        HMODULE module = LoadSystemDll(L"bcrypt.dll");
        if (!module) {
            return;
        }
#define RESOLVE(member, name) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name)); \
        if (!member) return;
        RESOLVE(OpenAlgorithmProvider, "BCryptOpenAlgorithmProvider")
        RESOLVE(CloseAlgorithmProvider, "BCryptCloseAlgorithmProvider")
        RESOLVE(CreateHash, "BCryptCreateHash")
        RESOLVE(HashData, "BCryptHashData")
        RESOLVE(FinishHash, "BCryptFinishHash")
        RESOLVE(DestroyHash, "BCryptDestroyHash")
        RESOLVE(ImportKeyPair, "BCryptImportKeyPair")
        RESOLVE(VerifySignature, "BCryptVerifySignature")
        RESOLVE(DestroyKey, "BCryptDestroyKey")
#undef RESOLVE
        ok = true;
    }
};

const WinHttpApi& Http() {
    static const WinHttpApi api;  // thread-safe one-time load
    return api;
}

const BCryptApi& Crypto() {
    static const BCryptApi api;
    return api;
}

//=============================================================================
// Small helpers
//=============================================================================

std::wstring Win32Error(const std::wstring& context, DWORD code) {
    wchar_t text[64];
    swprintf_s(text, L" (Windows error %lu)", code);
    return context + text;
}

bool IsLowerHex(const std::wstring& s, size_t length) {
    if (s.size() != length) {
        return false;
    }
    for (wchar_t c : s) {
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
            return false;
        }
    }
    return true;
}

bool IsLowerHex(const std::string& s, size_t length) {
    return IsLowerHex(std::wstring(s.begin(), s.end()), length);
}

std::string Narrow(const std::wstring& ascii) {
    std::string out;
    out.reserve(ascii.size());
    for (wchar_t c : ascii) {
        out.push_back(static_cast<char>(c));  // callers only pass validated ASCII
    }
    return out;
}

bool IsAscii(const std::string& s) {
    for (unsigned char c : s) {
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c > 0x7e) {
            return false;
        }
    }
    return true;
}

bool DecodeBase64(const std::string& text, std::vector<uint8_t>& out) {
    static const char* const alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.clear();
    if (text.empty() || text.size() % 4 != 0) {
        return false;
    }
    uint32_t accumulator = 0;
    int bits = 0;
    size_t padding = 0;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '=') {
            padding++;
            if (i < text.size() - 2) {
                return false;  // padding only at the very end
            }
            continue;
        }
        if (padding) {
            return false;
        }
        const char* position = std::strchr(alphabet, c);
        if (!position || c == '\0') {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(position - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    return true;
}

bool Sha256Digest(const uint8_t* data, size_t size, uint8_t digest[32]) {
    const BCryptApi& api = Crypto();
    if (!api.ok) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!NT_SUCCESS(api.OpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    bool ok = false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (NT_SUCCESS(api.CreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        ok = true;
        size_t offset = 0;
        while (ok && offset < size) {
            ULONG chunk = static_cast<ULONG>(std::min<size_t>(size - offset, 1u << 20));
            ok = NT_SUCCESS(api.HashData(hash, const_cast<PUCHAR>(data + offset), chunk, 0));
            offset += chunk;
        }
        if (ok) {
            ok = NT_SUCCESS(api.FinishHash(hash, digest, 32, 0));
        }
        api.DestroyHash(hash);
    }
    api.CloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::string ToHex(const uint8_t* data, size_t size) {
    static const char* const digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2);
    for (size_t i = 0; i < size; i++) {
        hex.push_back(digits[data[i] >> 4]);
        hex.push_back(digits[data[i] & 0x0f]);
    }
    return hex;
}

// One HTTPS GET with a byte cap. Redirects are followed by hand so every hop is checked
// against the host allow list; the response body is read until the server stops, counting
// bytes rather than trusting Content-Length.
bool HttpGet(const std::wstring& startUrl, size_t maxBytes, std::vector<uint8_t>& out, std::wstring& error) {
    const WinHttpApi& api = Http();
    if (!api.ok) {
        error = L"WinHTTP is not available on this computer";
        return false;
    }

    HINTERNET session = api.Open(L"AudioCapture-updater/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = Win32Error(L"could not open an HTTPS session", GetLastError());
        return false;
    }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!api.SetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // this Windows does not know TLS 1.3
        if (!api.SetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            error = Win32Error(L"could not require TLS 1.2", GetLastError());
            api.Close(session);
            return false;
        }
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    api.SetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    api.SetTimeouts(session, 10000, 10000, 15000, 30000);

    std::wstring url = startUrl;
    bool ok = false;
    error.clear();

    for (int hop = 0; hop <= kMaxRedirects && !ok; hop++) {
        std::wstring host;
        std::wstring path;
        if (!ParseUrl(url, host, path)) {
            error = L"the download address is not a plain https address";
            break;
        }
        if (!IsAllowedHost(host)) {
            error = L"refused to contact " + host + L"; only GitHub's own hosts are allowed";
            break;
        }

        HINTERNET connection = api.Connect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection) {
            error = Win32Error(L"could not connect to " + host, GetLastError());
            break;
        }
        HINTERNET request = api.OpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) {
            error = Win32Error(L"could not prepare the request", GetLastError());
            api.Close(connection);
            break;
        }
        api.SetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        if (!api.SendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !api.ReceiveResponse(request, nullptr)) {
            error = Win32Error(L"could not reach " + host + L" securely", GetLastError());
            api.Close(request);
            api.Close(connection);
            break;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        api.QueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            wchar_t location[2048] = {};
            DWORD locationSize = sizeof(location) - sizeof(wchar_t);
            bool haveLocation = api.QueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                                 location, &locationSize, WINHTTP_NO_HEADER_INDEX) != FALSE;
            api.Close(request);
            api.Close(connection);
            std::wstring next;
            if (!haveLocation || !ResolveRedirect(url, location, next)) {
                error = L"the server sent a redirect this program does not follow";
                break;
            }
            url = next;  // the next loop turn re-checks the host
            continue;
        }

        if (status != 200) {
            error = L"GitHub answered with HTTP " + std::to_wstring(status);
            api.Close(request);
            api.Close(connection);
            break;
        }

        out.clear();
        uint8_t buffer[16384];
        bool readOk = true;
        for (;;) {
            DWORD got = 0;
            if (!api.ReadData(request, buffer, sizeof(buffer), &got)) {
                error = Win32Error(L"the download stopped", GetLastError());
                readOk = false;
                break;
            }
            if (got == 0) {
                break;
            }
            if (out.size() + got > maxBytes) {
                error = L"the download is larger than the release information allows";
                readOk = false;
                break;
            }
            out.insert(out.end(), buffer, buffer + got);
        }
        api.Close(request);
        api.Close(connection);
        ok = readOk;
        break;
    }

    if (!ok && error.empty()) {
        error = L"too many redirects";
    }
    api.Close(session);
    return ok;
}

bool ExePaths(std::wstring& exePath, std::wstring& directory, std::wstring& stem, std::wstring& error) {
    wchar_t module[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = L"could not find the program's own path";
        return false;
    }
    exePath = module;
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        error = L"the program's path has no folder";
        return false;
    }
    directory = exePath.substr(0, slash + 1);
    std::wstring file = exePath.substr(slash + 1);
    stem = file;
    if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".exe") == 0) {
        stem.resize(stem.size() - 4);
    }
    return true;
}

}  // namespace

//=============================================================================
// Pure pieces (also exercised by tools/updater_tests.cpp)
//=============================================================================

BuildInfo RunningBuild() {
    BuildInfo info;
    info.commit = AUDIOCAPTURE_BUILD_COMMIT;
    info.commitDate = AUDIOCAPTURE_BUILD_COMMIT_DATE;
    return info;
}

bool ParseUrl(const std::wstring& url, std::wstring& host, std::wstring& path) {
    static const wchar_t* const prefix = L"https://";
    const size_t prefixLength = 8;
    if (url.size() <= prefixLength || _wcsnicmp(url.c_str(), prefix, prefixLength) != 0) {
        return false;
    }
    size_t slash = url.find(L'/', prefixLength);
    host = url.substr(prefixLength, slash == std::wstring::npos ? std::wstring::npos : slash - prefixLength);
    path = (slash == std::wstring::npos) ? L"/" : url.substr(slash);
    if (host.empty() || host.find_first_of(L"@:\\?#[] ") != std::wstring::npos) {
        return false;  // no user info, no port, nothing odd
    }
    for (wchar_t& c : host) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        } else if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.')) {
            return false;
        }
    }
    return true;
}

bool IsAllowedHost(const std::wstring& host) {
    if (host == L"github.com") {
        return true;
    }
    static const wchar_t* const suffix = L".githubusercontent.com";
    const size_t suffixLength = 22;
    return host.size() > suffixLength &&
           host.compare(host.size() - suffixLength, suffixLength, suffix) == 0;
}

bool ResolveRedirect(const std::wstring& currentUrl, const std::wstring& location, std::wstring& next) {
    if (location.empty()) {
        return false;
    }
    if (location[0] == L'/') {
        std::wstring host;
        std::wstring path;
        if (!ParseUrl(currentUrl, host, path)) {
            return false;
        }
        next = L"https://" + host + location;
        return true;
    }
    std::wstring host;
    std::wstring path;
    if (!ParseUrl(location, host, path)) {
        return false;  // anything but an absolute https address, including http, is refused
    }
    next = location;
    return true;
}

bool ParseIso8601(const std::wstring& text, int64_t& unixSeconds) {
    // YYYY-MM-DDTHH:MM:SS followed by Z or +HH:MM / -HH:MM (what git's %cI produces)
    int year, month, day, hour, minute, second;
    wchar_t separator;
    int consumed = 0;
    if (swscanf_s(text.c_str(), L"%4d-%2d-%2d%c%2d:%2d:%2d%n", &year, &month, &day, &separator, 1,
                  &hour, &minute, &second, &consumed) != 7 || (separator != L'T' && separator != L' ')) {
        return false;
    }
    std::wstring rest = text.substr(static_cast<size_t>(consumed));
    int offsetMinutes = 0;
    if (rest == L"Z") {
        offsetMinutes = 0;
    } else {
        int offsetHours, offsetMins;
        wchar_t sign;
        int used = 0;
        if (swscanf_s(rest.c_str(), L"%c%2d:%2d%n", &sign, 1, &offsetHours, &offsetMins, &used) != 3 ||
            (sign != L'+' && sign != L'-') || used != static_cast<int>(rest.size())) {
            return false;
        }
        offsetMinutes = offsetHours * 60 + offsetMins;
        if (sign == L'-') {
            offsetMinutes = -offsetMinutes;
        }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    // days from civil (Howard Hinnant), valid for the proleptic Gregorian calendar
    int y = year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153u * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(day) - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    unixSeconds = days * 86400 + hour * 3600 + minute * 60 + second - static_cast<int64_t>(offsetMinutes) * 60;
    return true;
}

bool ParseManifest(const std::string& text, Manifest& out, std::wstring& error) {
    using nlohmann::json;
    if (!IsAscii(text)) {
        error = L"it contains characters it should not";
        return false;
    }
    json document = json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = L"it is not a JSON object";
        return false;
    }
    if (!document.contains("schema") || !document["schema"].is_number_integer() || document["schema"].get<int>() != 1) {
        error = L"it uses a schema this program does not know";
        return false;
    }
    if (!document.contains("commit") || !document["commit"].is_string() ||
        !document.contains("commit_date") || !document["commit_date"].is_string() ||
        !document.contains("exe") || !document["exe"].is_object()) {
        error = L"it is missing fields";
        return false;
    }
    const json& exe = document["exe"];
    if (!exe.contains("name") || !exe["name"].is_string() || exe["name"].get<std::string>() != "AudioCapture.exe" ||
        !exe.contains("size") || !exe["size"].is_number_unsigned() ||
        !exe.contains("sha256") || !exe["sha256"].is_string() ||
        !exe.contains("signature") || !exe["signature"].is_string()) {
        error = L"its program entry is incomplete";
        return false;
    }

    std::string commit = document["commit"].get<std::string>();
    std::string commitDate = document["commit_date"].get<std::string>();
    std::string sha = exe["sha256"].get<std::string>();
    uint64_t size = exe["size"].get<uint64_t>();
    int64_t ignored = 0;

    if (!IsLowerHex(commit, 40)) {
        error = L"the commit is not 40 hex digits";
        return false;
    }
    if (!ParseIso8601(std::wstring(commitDate.begin(), commitDate.end()), ignored)) {
        error = L"the commit date is not a valid time";
        return false;
    }
    if (size < kMinExeBytes || size > kMaxExeBytes) {
        error = L"the program size is outside the expected range";
        return false;
    }
    if (!IsLowerHex(sha, 64)) {
        error = L"the SHA-256 is not 64 hex digits";
        return false;
    }
    std::vector<uint8_t> signature;
    if (!DecodeBase64(exe["signature"].get<std::string>(), signature) || signature.size() != 64) {
        error = L"the signature is not a 64-byte P-256 signature";
        return false;
    }

    out.commit = std::wstring(commit.begin(), commit.end());
    out.commitDate = std::wstring(commitDate.begin(), commitDate.end());
    out.exeSize = size;
    out.exeSha256 = sha;
    out.signature = signature;
    return true;
}

std::string CanonicalSigningText(const Manifest& manifest) {
    return "AudioCapture build-info v1\ncommit=" + Narrow(manifest.commit) +
           "\ncommit_date=" + Narrow(manifest.commitDate) +
           "\nexe_size=" + std::to_string(manifest.exeSize) +
           "\nexe_sha256=" + manifest.exeSha256 + "\n";
}

bool VerifyManifestSignature(const Manifest& manifest, const uint8_t* publicKeyBlob, size_t blobSize,
                             std::wstring& error) {
    const BCryptApi& api = Crypto();
    if (!api.ok) {
        error = L"Windows cryptography is not available";
        return false;
    }
    if (manifest.signature.size() != 64) {
        error = L"the signature has the wrong length";
        return false;
    }
    std::string text = CanonicalSigningText(manifest);
    uint8_t digest[32];
    if (!Sha256Digest(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest)) {
        error = L"could not hash the release information";
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!NT_SUCCESS(api.OpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        error = L"ECDSA P-256 is not available";
        return false;
    }
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = api.ImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                        const_cast<PUCHAR>(publicKeyBlob), static_cast<ULONG>(blobSize), 0);
    if (!NT_SUCCESS(status)) {
        api.CloseAlgorithmProvider(algorithm, 0);
        error = L"the built-in public key could not be loaded";
        return false;
    }
    std::vector<uint8_t> signature = manifest.signature;
    status = api.VerifySignature(key, nullptr, digest, 32, signature.data(), static_cast<ULONG>(signature.size()), 0);
    api.DestroyKey(key);
    api.CloseAlgorithmProvider(algorithm, 0);
    if (!NT_SUCCESS(status)) {
        error = L"the signature does not match";
        return false;
    }
    return true;
}

CheckResult CompareBuilds(const BuildInfo& running, const Manifest& published) {
    if (!running.commit.empty() && running.commit != L"unknown" && published.commit == running.commit) {
        return CheckResult::UpToDate;
    }
    int64_t publishedTime = 0;
    int64_t runningTime = 0;
    if (ParseIso8601(published.commitDate, publishedTime) && ParseIso8601(running.commitDate, runningTime) &&
        publishedTime <= runningTime) {
        return CheckResult::PublishedIsOlder;
    }
    return CheckResult::UpdateAvailable;
}

bool Sha256Hex(const uint8_t* data, size_t size, std::string& hexOut) {
    uint8_t digest[32];
    if (!Sha256Digest(data, size, digest)) {
        return false;
    }
    hexOut = ToHex(digest, 32);
    return true;
}

//=============================================================================
// The two operations the window calls
//=============================================================================

CheckOutcome CheckForUpdate() {
    CheckOutcome outcome;
    std::vector<uint8_t> body;
    std::wstring error;
    if (!HttpGet(kManifestUrl, kMaxManifestBytes, body, error)) {
        outcome.message = L"Could not fetch the release information: " + error + L".";
        return outcome;
    }
    std::string text(body.begin(), body.end());
    if (!ParseManifest(text, outcome.manifest, error)) {
        outcome.message = L"The release information is not usable: " + error + L". Nothing was changed.";
        return outcome;
    }
    if (!VerifyManifestSignature(outcome.manifest, kPublicKeyBlob, sizeof(kPublicKeyBlob), error)) {
        outcome.message = L"The release information is not signed by this project's key: " + error +
                          L". Nothing was changed.";
        return outcome;
    }
    outcome.result = CompareBuilds(RunningBuild(), outcome.manifest);
    return outcome;
}

bool DownloadAndInstall(const Manifest& manifest, std::wstring& error) {
    std::vector<uint8_t> exe;
    if (!HttpGet(kExeUrl, static_cast<size_t>(manifest.exeSize), exe, error)) {
        return false;
    }
    if (exe.size() != manifest.exeSize) {
        error = L"the downloaded file has the wrong size";
        return false;
    }
    if (exe.size() < 2 || exe[0] != 'M' || exe[1] != 'Z') {
        error = L"the downloaded file is not a Windows program";
        return false;
    }
    std::string sha;
    if (!Sha256Hex(exe.data(), exe.size(), sha) || sha != manifest.exeSha256) {
        error = L"the downloaded file does not match the signed SHA-256";
        return false;
    }

    std::wstring exePath;
    std::wstring directory;
    std::wstring stem;
    if (!ExePaths(exePath, directory, stem, error)) {
        return false;
    }
    std::wstring tempPath = directory + stem + L".update." + std::to_wstring(GetCurrentProcessId()) + L"." +
                            std::to_wstring(GetTickCount64()) + L".exe";
    std::wstring oldPath = directory + stem + L".old.exe";

    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Win32Error(L"could not create a file next to the program in " + directory, GetLastError());
        return false;
    }
    size_t written = 0;
    bool writeOk = true;
    while (writeOk && written < exe.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(exe.size() - written, 1u << 20));
        DWORD done = 0;
        writeOk = WriteFile(file, exe.data() + written, chunk, &done, nullptr) && done == chunk;
        written += done;
    }
    if (writeOk) {
        writeOk = FlushFileBuffers(file) != FALSE;
    }
    DWORD writeError = GetLastError();
    CloseHandle(file);
    if (!writeOk) {
        DeleteFileW(tempPath.c_str());
        error = Win32Error(L"could not write the new program file", writeError);
        return false;
    }

    DeleteFileW(oldPath.c_str());  // leftover from an earlier update; failure is fine
    if (!MoveFileExW(exePath.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = Win32Error(L"could not move the running program aside", GetLastError());
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = Win32Error(L"could not put the new program in place", GetLastError());
        MoveFileExW(oldPath.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING);  // put the old one back
        DeleteFileW(tempPath.c_str());
        return false;
    }

    std::wstring commandLine = L"\"" + exePath + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(exePath.c_str(), &commandLine[0], nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &startup, &process)) {
        error = Win32Error(L"the new build is installed but could not be started; close and reopen Audio Capture",
                           GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void RemoveLeftoverOldExe() {
    std::wstring exePath;
    std::wstring directory;
    std::wstring stem;
    std::wstring error;
    if (ExePaths(exePath, directory, stem, error)) {
        DeleteFileW((directory + stem + L".old.exe").c_str());
    }
}

}  // namespace updater
