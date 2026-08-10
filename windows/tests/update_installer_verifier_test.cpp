#include "update/update_installer_verifier_win32.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using codex_monitor::update::ConstantTimeSha256Equals;
using codex_monitor::update::IsAllowedWindowsInstallerFileSize;
using codex_monitor::update::ParseWindowsInstallerSha256Manifest;
using codex_monitor::update::Sha256Digest;
using codex_monitor::update::Sha256ManifestParseStatus;
using codex_monitor::update::VerifyDownloadedWindowsInstallerChecksum;
using codex_monitor::update::WindowsInstallerVerificationStatus;

constexpr char kInstallerName[] =
    "CodexMonitorHUD-windows-x64-1.2.3.msi";
constexpr char kAbcSha256[] =
    "ba7816bf8f01cfea414140de5dae2223"
    "b00361a396177a9cb410ff61f20015ad";

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string Manifest(std::string hash = kAbcSha256,
                     std::string fileName = kInstallerName,
                     std::string newline = "\n") {
    return hash + "  " + fileName + newline;
}

void TestStrictManifestParsing() {
    const auto valid = ParseWindowsInstallerSha256Manifest(
        Manifest(), kInstallerName);
    Require(valid.valid() && valid.digest.front() == 0xba &&
                valid.digest.back() == 0xad,
            "the generated one-line manifest must parse");
    Require(ParseWindowsInstallerSha256Manifest(
                Manifest(kAbcSha256, kInstallerName, ""),
                kInstallerName).valid(),
            "a manifest without a final newline must parse");
    Require(ParseWindowsInstallerSha256Manifest(
                Manifest(kAbcSha256, kInstallerName, "\r\n"),
                kInstallerName).valid(),
            "a CRLF-terminated manifest must parse");

    std::string uppercase = kAbcSha256;
    for (char& character : uppercase) {
        if (character >= 'a' && character <= 'f') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    Require(ParseWindowsInstallerSha256Manifest(
                Manifest(uppercase), kInstallerName).valid(),
            "hexadecimal digest case must not change its value");

    const std::string wrongName =
        "CodexMonitorHUD-windows-x64-1.2.4.msi";
    Require(ParseWindowsInstallerSha256Manifest(
                Manifest(kAbcSha256, wrongName), kInstallerName).status ==
                Sha256ManifestParseStatus::kFileNameMismatch,
            "the manifest file name must exactly match the selected MSI");
    Require(ParseWindowsInstallerSha256Manifest(
                Manifest(),
                "../CodexMonitorHUD-windows-x64-1.2.3.msi").status ==
                Sha256ManifestParseStatus::kInvalidExpectedFileName,
            "an expected file name with a path component must be rejected");
}

void TestMalformedManifestRejection() {
    std::string badHex = kAbcSha256;
    badHex[17] = 'g';
    const std::string malformed[] = {
        Manifest(badHex),
        std::string(kAbcSha256) + " " + kInstallerName + "\n",
        std::string(kAbcSha256) + " *" + kInstallerName + "\n",
        Manifest() + "extra\n",
        Manifest(kAbcSha256, kInstallerName, "\r"),
        Manifest(std::string(63, '0')),
        std::string("\xEF\xBB\xBF") + Manifest(),
    };
    for (const std::string& value : malformed) {
        Require(!ParseWindowsInstallerSha256Manifest(
                     value, kInstallerName).valid(),
                "extra lines, whitespace, markers, BOM, and bad hex must fail");
    }

    Require(ParseWindowsInstallerSha256Manifest(
                std::string(
                    codex_monitor::update::kMaximumSha256ManifestBytes + 1,
                    'x'),
                kInstallerName).status ==
                Sha256ManifestParseStatus::kTooLarge,
            "oversized manifests must fail before parsing");
}

void TestConstantTimeDigestComparison() {
    const auto parsed = ParseWindowsInstallerSha256Manifest(
        Manifest(), kInstallerName);
    Require(parsed.valid(), "comparison fixture must parse");
    Sha256Digest changedFirst = parsed.digest;
    Sha256Digest changedLast = parsed.digest;
    changedFirst.front() ^= 0xff;
    changedLast.back() ^= 0xff;
    Require(ConstantTimeSha256Equals(parsed.digest, parsed.digest),
            "equal SHA-256 digests must compare equal");
    Require(!ConstantTimeSha256Equals(parsed.digest, changedFirst) &&
                !ConstantTimeSha256Equals(parsed.digest, changedLast),
            "differences at either end of the digest must be detected");
}

void TestInstallerSizeLimit() {
    Require(!IsAllowedWindowsInstallerFileSize(0),
            "an empty installer must be rejected");
    Require(IsAllowedWindowsInstallerFileSize(1) &&
                IsAllowedWindowsInstallerFileSize(
                    codex_monitor::update::kMaximumWindowsInstallerBytes),
            "non-empty installers through the hard limit must be accepted");
    Require(!IsAllowedWindowsInstallerFileSize(
                codex_monitor::update::kMaximumWindowsInstallerBytes + 1),
            "an installer above the 256 MiB hard limit must be rejected");
}

#ifdef _WIN32

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("codex-update-verifier-" +
                 std::to_string(static_cast<unsigned long long>(
                     std::filesystem::file_time_type::clock::now()
                         .time_since_epoch().count())));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        Require(!error, "the temporary verifier directory must be created");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void Write(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    Require(static_cast<bool>(output), "the verifier fixture must be written");
}

void TestWindowsCngVerification() {
    TemporaryDirectory temporary;
    const std::filesystem::path installer =
        temporary.path() / kInstallerName;
    Write(installer, "abc");

    const auto verified = VerifyDownloadedWindowsInstallerChecksum(
        installer, kInstallerName, Manifest());
    Require(verified.status ==
                WindowsInstallerVerificationStatus::kChecksumVerified &&
                verified.fileSizeBytes == 3,
            "the CNG SHA-256 path must verify a known file vector");

    std::string wrongHash(64, '0');
    Require(VerifyDownloadedWindowsInstallerChecksum(
                installer, kInstallerName, Manifest(wrongHash)).status ==
                WindowsInstallerVerificationStatus::kDigestMismatch,
            "a checksum mismatch must fail closed");

    const std::filesystem::path wrongTarget =
        temporary.path() /
        "CodexMonitorHUD-windows-x64-1.2.4.msi";
    Write(wrongTarget, "abc");
    Require(VerifyDownloadedWindowsInstallerChecksum(
                wrongTarget, kInstallerName, Manifest()).status ==
                WindowsInstallerVerificationStatus::
                    kTargetFileNameMismatch,
            "the on-disk target name must exactly match the selected MSI");

    Write(installer, "");
    Require(VerifyDownloadedWindowsInstallerChecksum(
                installer, kInstallerName, Manifest()).status ==
                WindowsInstallerVerificationStatus::kEmptyFile,
            "an empty installer must not verify");
}

#endif

}  // namespace

int main() {
    TestStrictManifestParsing();
    TestMalformedManifestRejection();
    TestConstantTimeDigestComparison();
    TestInstallerSizeLimit();
#ifdef _WIN32
    TestWindowsCngVerification();
#endif
    std::cout << "update_installer_verifier_tests=pass\n";
    return 0;
}
