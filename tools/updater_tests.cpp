// Checks the updater's pure pieces without any network: the host allow list, redirect handling,
// time parsing, manifest validation, the downgrade rule, and ECDSA verification against a key
// pair made on the spot (good signature, flipped bit, and tampered metadata).
//
// usage: updater_tests.exe   (exit code is the number of failed checks)

#include "Updater.h"
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

static int g_failures = 0;

static void Check(bool condition, const char* what) {
    printf("%s  %s\n", condition ? "pass" : "FAIL", what);
    if (!condition) {
        g_failures++;
    }
}

static std::string Base64(const std::vector<uint8_t>& data) {
    static const char* const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (i + 1 == data.size()) {
        uint32_t n = data[i] << 16;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

static std::string ManifestJson(const std::string& commit, const std::string& date, uint64_t size,
                                const std::string& sha, const std::string& signatureBase64,
                                const std::string& name = "AudioCapture.exe") {
    return "{\"schema\":1,\"commit\":\"" + commit + "\",\"commit_date\":\"" + date +
           "\",\"exe\":{\"name\":\"" + name + "\",\"size\":" + std::to_string(size) +
           ",\"sha256\":\"" + sha + "\",\"signature\":\"" + signatureBase64 + "\"}}";
}

int main() {
    using namespace updater;
    std::wstring host, path, next, error;

    // --- addresses -------------------------------------------------------------------------
    Check(ParseUrl(L"https://github.com/a/b?c=d", host, path) && host == L"github.com" && path == L"/a/b?c=d",
          "ParseUrl keeps host and path");
    Check(ParseUrl(L"https://GitHub.com", host, path) && host == L"github.com" && path == L"/",
          "ParseUrl lowercases the host and defaults the path");
    Check(!ParseUrl(L"http://github.com/x", host, path), "ParseUrl refuses http");
    Check(!ParseUrl(L"https://user@github.com/x", host, path), "ParseUrl refuses user info");
    Check(!ParseUrl(L"https://github.com:8443/x", host, path), "ParseUrl refuses a port");
    Check(IsAllowedHost(L"github.com"), "github.com allowed");
    Check(IsAllowedHost(L"release-assets.githubusercontent.com"), "release-assets host allowed");
    Check(IsAllowedHost(L"objects.githubusercontent.com"), "objects host allowed");
    Check(!IsAllowedHost(L"api.github.com"), "api.github.com not allowed");
    Check(!IsAllowedHost(L"githubusercontent.com"), "bare githubusercontent.com not allowed");
    Check(!IsAllowedHost(L"evil-githubusercontent.com"), "look-alike suffix not allowed");
    Check(!IsAllowedHost(L"github.com.evil.net"), "github.com as a prefix not allowed");
    Check(ResolveRedirect(L"https://github.com/x", L"https://objects.githubusercontent.com/y", next) &&
              next == L"https://objects.githubusercontent.com/y",
          "absolute https redirect accepted (host re-checked by the caller)");
    Check(ResolveRedirect(L"https://github.com/x", L"/relative", next) && next == L"https://github.com/relative",
          "relative redirect stays on the same host");
    Check(!ResolveRedirect(L"https://github.com/x", L"http://github.com/y", next), "http redirect refused");
    Check(!ResolveRedirect(L"https://github.com/x", L"", next), "empty redirect refused");

    // --- time -------------------------------------------------------------------------------
    int64_t a = 0, b = 0, c = 0;
    Check(ParseIso8601(L"2026-10-11T06:20:00+00:00", a) && ParseIso8601(L"2026-10-11T06:20:00Z", b) && a == b,
          "ISO 8601 +00:00 equals Z");
    Check(ParseIso8601(L"2026-10-11T08:20:00+02:00", c) && c == a, "ISO 8601 offsets are applied");
    Check(a == 1791699600, "ISO 8601 maps to the right Unix time");
    Check(!ParseIso8601(L"2026-10-11", a), "date without time refused");
    Check(!ParseIso8601(L"2026-10-11T06:20:00+02", a), "truncated offset refused");

    // --- a key pair made on the spot, to sign a manifest the way CI does ------------------------
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool keyOk = NT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) &&
                 NT_SUCCESS(BCryptGenerateKeyPair(algorithm, &key, 256, 0)) &&
                 NT_SUCCESS(BCryptFinalizeKeyPair(key, 0));
    Check(keyOk, "test key pair generated");

    std::vector<uint8_t> publicBlob(72);
    ULONG blobSize = 0;
    Check(keyOk && NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, publicBlob.data(), 72, &blobSize, 0)) &&
              blobSize == 72,
          "public blob exported (72 bytes)");

    const std::string commit = "d830bdcf3f3f27dda97da269227bd59d63c18b1f";
    const std::string date = "2026-10-11T06:20:00+00:00";
    const uint64_t size = 1070080;
    const std::string sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    Manifest unsignedManifest;
    unsignedManifest.commit = std::wstring(commit.begin(), commit.end());
    unsignedManifest.commitDate = std::wstring(date.begin(), date.end());
    unsignedManifest.exeSize = size;
    unsignedManifest.exeSha256 = sha;
    std::string canonical = CanonicalSigningText(unsignedManifest);
    Check(canonical == "AudioCapture build-info v1\ncommit=" + commit + "\ncommit_date=" + date +
                           "\nexe_size=1070080\nexe_sha256=" + sha + "\n",
          "canonical signing text has the agreed layout");

    std::string canonicalSha;
    Check(Sha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3, canonicalSha) &&
              canonicalSha == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of \"abc\" is right");

    uint8_t digest[32] = {};
    {
        BCRYPT_ALG_HANDLE sha256 = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(sha256, &hash, nullptr, 0, nullptr, 0, 0);
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(&canonical[0]), static_cast<ULONG>(canonical.size()), 0);
        BCryptFinishHash(hash, digest, 32, 0);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(sha256, 0);
    }
    std::vector<uint8_t> signature(64);
    ULONG signatureSize = 0;
    Check(keyOk && NT_SUCCESS(BCryptSignHash(key, nullptr, digest, 32, signature.data(), 64, &signatureSize, 0)) &&
              signatureSize == 64,
          "signed the canonical text (64-byte r||s)");

    // --- manifest parsing ---------------------------------------------------------------------
    Manifest parsed;
    Check(ParseManifest(ManifestJson(commit, date, size, sha, Base64(signature)), parsed, error) &&
              parsed.commit == unsignedManifest.commit && parsed.exeSize == size && parsed.exeSha256 == sha &&
              parsed.signature == signature,
          "valid manifest parses");
    Check(!ParseManifest(ManifestJson("abc", date, size, sha, Base64(signature)), parsed, error), "short commit refused");
    Check(!ParseManifest(ManifestJson(commit, "yesterday", size, sha, Base64(signature)), parsed, error), "bad date refused");
    Check(!ParseManifest(ManifestJson(commit, date, 50ull * 1024 * 1024, sha, Base64(signature)), parsed, error),
          "oversized exe refused");
    Check(!ParseManifest(ManifestJson(commit, date, size, "zz", Base64(signature)), parsed, error), "bad sha refused");
    Check(!ParseManifest(ManifestJson(commit, date, size, sha, "AAAA"), parsed, error), "short signature refused");
    Check(!ParseManifest(ManifestJson(commit, date, size, sha, Base64(signature), "evil.exe"), parsed, error),
          "wrong asset name refused");
    Check(!ParseManifest("[1,2,3]", parsed, error), "non-object refused");
    Check(!ParseManifest("{\"schema\":2}", parsed, error), "unknown schema refused");

    // --- signature verification ---------------------------------------------------------------
    Manifest good;
    ParseManifest(ManifestJson(commit, date, size, sha, Base64(signature)), good, error);
    Check(VerifyManifestSignature(good, publicBlob.data(), publicBlob.size(), error), "good signature verifies");

    Manifest flipped = good;
    flipped.signature[10] ^= 0x01;
    Check(!VerifyManifestSignature(flipped, publicBlob.data(), publicBlob.size(), error), "flipped bit fails");

    Manifest replayed = good;
    replayed.commitDate = L"2030-01-01T00:00:00+00:00";  // same exe, forged newer date
    Check(!VerifyManifestSignature(replayed, publicBlob.data(), publicBlob.size(), error),
          "metadata is covered by the signature (forged date fails)");

    Manifest otherSize = good;
    otherSize.exeSize = size + 1;
    Check(!VerifyManifestSignature(otherSize, publicBlob.data(), publicBlob.size(), error), "changed size fails");

    uint8_t wrongKey[72];
    memcpy(wrongKey, publicBlob.data(), 72);
    wrongKey[20] ^= 0x01;
    Check(!VerifyManifestSignature(good, wrongKey, 72, error), "a different key fails");

    // --- downgrade rule -----------------------------------------------------------------------
    BuildInfo running;
    running.commit = good.commit;
    running.commitDate = good.commitDate;
    Check(CompareBuilds(running, good) == CheckResult::UpToDate, "same commit is up to date");

    BuildInfo newerRunning;
    newerRunning.commit = L"ffffffffffffffffffffffffffffffffffffffff";
    newerRunning.commitDate = L"2026-10-12T00:00:00+00:00";
    Check(CompareBuilds(newerRunning, good) == CheckResult::PublishedIsOlder, "older published build refused");

    BuildInfo olderRunning;
    olderRunning.commit = L"ffffffffffffffffffffffffffffffffffffffff";
    olderRunning.commitDate = L"2026-10-10T00:00:00+00:00";
    Check(CompareBuilds(olderRunning, good) == CheckResult::UpdateAvailable, "newer published build offered");

    BuildInfo unknownRunning;
    unknownRunning.commit = L"unknown";
    Check(CompareBuilds(unknownRunning, good) == CheckResult::UpdateAvailable, "unknown local build is offered the update");

    if (key) {
        BCryptDestroyKey(key);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    printf("%d check(s) failed\n", g_failures);
    return g_failures;
}
