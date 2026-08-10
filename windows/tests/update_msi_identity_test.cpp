#include "update/update_msi_identity_win32.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using codex_monitor::update::PublisherCertificateSha256;
using codex_monitor::update::ValidateWindowsMsiIdentity;
using codex_monitor::update::VerifyWindowsMsiIdentityAndPublisher;
using codex_monitor::update::WindowsMsiIdentity;
using codex_monitor::update::WindowsMsiIdentityPolicyStatus;
using codex_monitor::update::WindowsMsiIdentityVerificationStatus;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

WindowsMsiIdentity ValidIdentity(std::wstring version = L"1.2.3") {
    WindowsMsiIdentity identity;
    identity.productName = L"Codex Monitor HUD";
    identity.productVersion = std::move(version);
    identity.upgradeCode =
        L"{0CA9E00B-2AAF-4393-B466-1AF0F8C2C21F}";
    identity.templateValue = L"x64;1033";
    return identity;
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<PublisherCertificateSha256> ParsePublisherFingerprint(
    std::string_view value) noexcept {
    if (value.size() != PublisherCertificateSha256{}.size() * 2U) {
        return std::nullopt;
    }
    PublisherCertificateSha256 fingerprint{};
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        const int high = HexValue(value[index * 2U]);
        const int low = HexValue(value[index * 2U + 1U]);
        if (high < 0 || low < 0) return std::nullopt;
        fingerprint[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return fingerprint;
}

void TestPublisherFingerprintParser() {
    const std::string zeroes(64, '0');
    const auto zeroFingerprint = ParsePublisherFingerprint(zeroes);
    Require(zeroFingerprint.has_value(),
            "an exact 64-character hexadecimal fingerprint must parse");
    Require(zeroFingerprint->front() == 0 && zeroFingerprint->back() == 0,
            "hexadecimal zeroes must retain their byte value");

    const std::string mixedCase =
        "0123456789abcdef0123456789ABCDEF"
        "fedcba9876543210FEDCBA9876543210";
    const auto mixedFingerprint = ParsePublisherFingerprint(mixedCase);
    Require(mixedFingerprint.has_value(),
            "both hexadecimal letter cases must be accepted");
    Require(mixedFingerprint->front() == 0x01 &&
                mixedFingerprint->back() == 0x10,
            "hexadecimal pairs must map to the expected bytes");

    Require(!ParsePublisherFingerprint(std::string(63, '0')).has_value(),
            "a short publisher fingerprint must be rejected");
    Require(!ParsePublisherFingerprint(std::string(65, '0')).has_value(),
            "a long publisher fingerprint must be rejected");
    std::string invalid = zeroes;
    invalid.back() = 'g';
    Require(!ParsePublisherFingerprint(invalid).has_value(),
            "a non-hexadecimal publisher fingerprint must be rejected");
}

void TestValidIdentityPolicy() {
    Require(ValidateWindowsMsiIdentity(ValidIdentity(), "1.2.3") ==
                WindowsMsiIdentityPolicyStatus::kValid,
            "the exact product identity must be accepted");
    Require(ValidateWindowsMsiIdentity(
                ValidIdentity(L"255.255.65535"), "255.255.65535") ==
                WindowsMsiIdentityPolicyStatus::kValid,
            "the Windows Installer version limits must be accepted");
    Require(ValidateWindowsMsiIdentity(ValidIdentity(L"0.0.0"), "0.0.0") ==
                WindowsMsiIdentityPolicyStatus::kValid,
            "zero-valued canonical version parts must be accepted");
}

void TestExpectedVersionPolicy() {
    constexpr std::string_view invalidVersions[] = {
        "",          "1",          "1.2",       "1.2.3.4",
        "01.2.3",    "1.02.3",     "1.2.03",    "256.0.0",
        "0.256.0",   "0.0.65536",  "1.2.-1",    "1.2.3-beta",
        "1.2.3+meta", " 1.2.3",    "1.2.3 ",    "v1.2.3",
    };
    for (const std::string_view version : invalidVersions) {
        Require(ValidateWindowsMsiIdentity(ValidIdentity(), version) ==
                    WindowsMsiIdentityPolicyStatus::kInvalidExpectedVersion,
                "non-canonical or out-of-range expected versions must fail");
    }
}

void TestIdentityMismatches() {
    WindowsMsiIdentity identity = ValidIdentity();
    identity.productName = L"Codex monitor HUD";
    Require(ValidateWindowsMsiIdentity(identity, "1.2.3") ==
                WindowsMsiIdentityPolicyStatus::kProductNameMismatch,
            "ProductName comparison must be exact and case-sensitive");

    identity = ValidIdentity(L"1.2.4");
    Require(ValidateWindowsMsiIdentity(identity, "1.2.3") ==
                WindowsMsiIdentityPolicyStatus::kProductVersionMismatch,
            "ProductVersion must exactly equal the selected release");

    identity = ValidIdentity();
    identity.upgradeCode =
        L"0CA9E00B-2AAF-4393-B466-1AF0F8C2C21F";
    Require(ValidateWindowsMsiIdentity(identity, "1.2.3") ==
                WindowsMsiIdentityPolicyStatus::kUpgradeCodeMismatch,
            "UpgradeCode must retain its exact canonical braces");

    identity = ValidIdentity();
    identity.upgradeCode =
        L"{0ca9e00b-2aaf-4393-b466-1af0f8c2c21f}";
    Require(ValidateWindowsMsiIdentity(identity, "1.2.3") ==
                WindowsMsiIdentityPolicyStatus::kUpgradeCodeMismatch,
            "UpgradeCode comparison must be case-sensitive");

    identity = ValidIdentity();
    identity.templateValue = L"Intel64;1033";
    Require(ValidateWindowsMsiIdentity(identity, "1.2.3") ==
                WindowsMsiIdentityPolicyStatus::kTemplateMismatch,
            "the package template must identify x64 and language 1033");

    identity = ValidIdentity();
    identity.templateValue = L"x64;1033,2052";
    Require(ValidateWindowsMsiIdentity(identity, "1.2.3") ==
                WindowsMsiIdentityPolicyStatus::kTemplateMismatch,
            "additional package languages must not pass strict identity");
}

void TestVerifierFailsClosedBeforePlatformWork() {
    const std::filesystem::path missingInstaller =
        std::filesystem::temp_directory_path() /
        "definitely-missing-codex-monitor-update.msi";

    const auto missingFingerprint = VerifyWindowsMsiIdentityAndPublisher(
        missingInstaller, "1.2.3", std::nullopt);
    Require(missingFingerprint.status ==
                WindowsMsiIdentityVerificationStatus::
                    kMissingTrustedPublisherFingerprint,
            "the verifier must explicitly reject an absent publisher pin");

    PublisherCertificateSha256 fingerprint{};
    const auto relativePath = VerifyWindowsMsiIdentityAndPublisher(
        std::filesystem::path("relative-update.msi"), "1.2.3", fingerprint);
    Require(relativePath.status ==
                WindowsMsiIdentityVerificationStatus::kPathNotAbsolute,
            "the verifier must reject relative paths before platform IO");

    const auto invalidVersion = VerifyWindowsMsiIdentityAndPublisher(
        missingInstaller, "1.2", fingerprint);
    Require(invalidVersion.status ==
                WindowsMsiIdentityVerificationStatus::
                    kInvalidExpectedVersion,
            "the verifier must reject an invalid release version before IO");

    const auto platformResult = VerifyWindowsMsiIdentityAndPublisher(
        missingInstaller, "1.2.3", fingerprint);
#ifdef _WIN32
    Require(platformResult.status ==
                WindowsMsiIdentityVerificationStatus::kFileOpenFailed,
            "Windows must reject a nonexistent MSI before trust evaluation");

    const std::filesystem::path unsignedInstaller =
        std::filesystem::temp_directory_path() /
        ("codex-monitor-unsigned-" +
         std::to_string(static_cast<long long>(
             std::filesystem::file_time_type::clock::now()
                 .time_since_epoch().count())) +
         ".msi");
    {
        std::ofstream output(unsignedInstaller,
                             std::ios::binary | std::ios::trunc);
        output << "not a signed Windows Installer package";
        Require(static_cast<bool>(output),
                "the unsigned Windows fixture must be written");
    }
    const auto unsignedResult = VerifyWindowsMsiIdentityAndPublisher(
        unsignedInstaller, "1.2.3", fingerprint);
    std::error_code ignored;
    std::filesystem::remove(unsignedInstaller, ignored);
    Require(unsignedResult.status ==
                WindowsMsiIdentityVerificationStatus::
                    kSignatureVerificationFailed,
            "an unsigned file must fail before the MSI database is opened");
#else
    Require(platformResult.status ==
                WindowsMsiIdentityVerificationStatus::kUnsupportedPlatform,
            "portable builds must expose the unsupported verification path");
#endif
}

void TestAncestorReparsePointRejectionWhenSupported() {
#ifdef _WIN32
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("codex-monitor-reparse-test-" +
         std::to_string(static_cast<long long>(
             std::filesystem::file_time_type::clock::now()
                 .time_since_epoch().count())));
    const std::filesystem::path target = root / "target";
    const std::filesystem::path link = root / "link";
    std::error_code error;
    std::filesystem::create_directories(target, error);
    Require(!error, "the reparse test target directory must be created");
    std::filesystem::create_directory_symlink(target, link, error);
    if (error) {
        std::filesystem::remove_all(root, error);
        std::cout << "ancestor_reparse_test=skipped\n";
        return;
    }

    const std::filesystem::path installer = target / "candidate.msi";
    {
        std::ofstream output(installer, std::ios::binary | std::ios::trunc);
        output << "unsigned";
        Require(static_cast<bool>(output),
                "the reparse test installer must be written");
    }
    PublisherCertificateSha256 fingerprint{};
    const auto result = VerifyWindowsMsiIdentityAndPublisher(
        link / installer.filename(), "1.2.3", fingerprint);
    std::filesystem::remove_all(root, error);
    Require(result.status ==
                WindowsMsiIdentityVerificationStatus::kUnsafePathAncestor,
            "an ancestor reparse point must fail before signature checks");
#endif
}

int RunSignedMsiVerificationCli(int argc, char* argv[]) {
    if (argc != 6 || std::string_view(argv[1]) != "--verify-signed-msi") {
        std::cerr << "usage: --verify-signed-msi <absolute-msi> "
                     "<expected-version> <64hex-cert-sha256> "
                     "<verified|publisher-fingerprint-mismatch|"
                     "signature-verification-failed>\n";
        return 2;
    }

    std::filesystem::path installer;
    try {
        installer = std::filesystem::u8path(argv[2]);
    } catch (...) {
        std::cerr << "signed_msi_verification=invalid-msi-path\n";
        return 2;
    }
    if (!installer.is_absolute()) {
        std::cerr << "signed_msi_verification=invalid-absolute-path\n";
        return 2;
    }
    const std::optional<PublisherCertificateSha256> fingerprint =
        ParsePublisherFingerprint(argv[4]);
    if (!fingerprint.has_value()) {
        std::cerr << "signed_msi_verification=invalid-certificate-sha256\n";
        return 2;
    }

    WindowsMsiIdentityVerificationStatus expectedStatus =
        WindowsMsiIdentityVerificationStatus::kUnexpected;
    const std::string_view expectedResult(argv[5]);
    if (expectedResult == "verified") {
        expectedStatus = WindowsMsiIdentityVerificationStatus::kVerified;
    } else if (expectedResult == "publisher-fingerprint-mismatch") {
        expectedStatus = WindowsMsiIdentityVerificationStatus::
            kPublisherFingerprintMismatch;
    } else if (expectedResult == "signature-verification-failed") {
        expectedStatus = WindowsMsiIdentityVerificationStatus::
            kSignatureVerificationFailed;
    } else {
        std::cerr << "signed_msi_verification=invalid-expected-result\n";
        return 2;
    }

    const auto result = VerifyWindowsMsiIdentityAndPublisher(
        installer, argv[3], fingerprint);
    if (result.status != expectedStatus) {
        std::cerr << "signed_msi_verification=fail actual_status="
                  << static_cast<int>(result.status)
                  << " expected_status=" << static_cast<int>(expectedStatus)
                  << " policy_status="
                  << static_cast<int>(result.policyStatus) << '\n';
        return 1;
    }
    std::cout << "signed_msi_verification=pass status="
              << static_cast<int>(result.status) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 1) return RunSignedMsiVerificationCli(argc, argv);

    TestPublisherFingerprintParser();
    TestValidIdentityPolicy();
    TestExpectedVersionPolicy();
    TestIdentityMismatches();
    TestVerifierFailsClosedBeforePlatformWork();
    TestAncestorReparsePointRejectionWhenSupported();
    std::cout << "update_msi_identity_tests=pass\n";
    return 0;
}
