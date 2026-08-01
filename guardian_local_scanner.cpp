#include "guardian_local_scanner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include "settings.h"
#include "win_helpers.h"

namespace ss {
namespace {

#ifndef SONALIS_RELEASE_PUBLIC_KEY_BASE64
#define SONALIS_RELEASE_PUBLIC_KEY_BASE64 ""
#endif
#ifndef SONALIS_UPDATE_CHANNEL
#define SONALIS_UPDATE_CHANNEL "stable"
#endif

bool Base64(const std::string& text, std::vector<std::uint8_t>& output) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) return false;
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              nullptr, &bytes, nullptr, nullptr)) return false;
    output.resize(bytes);
    return CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                output.data(), &bytes, nullptr, nullptr) != FALSE;
}

std::string Sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                      sizeof(objectBytes), &written, 0);
    BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes),
                      sizeof(hashBytes), &written, 0);
    std::vector<std::uint8_t> object(objectBytes);
    std::array<std::uint8_t, 32> digest{};
    if (hashBytes != digest.size()
        || BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 64U * 1024U> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
    }
    const bool success = stream.eof()
        && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

std::string Sha256Text(const std::string_view text) {
    std::array<std::uint8_t, 32> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD written = 0;
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<ULONG>::max())
        || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                             &written, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<std::uint8_t> object(objectBytes);
    const bool success = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
                                          nullptr, 0, 0) >= 0
        && BCryptHashData(hash,
                          reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                          static_cast<ULONG>(text.size()), 0) >= 0
        && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

std::string Canonical(const GuardianClientModel& model) {
    return std::string{"sonalis-guardian-client-model-v1\n"}
        + model.platform + '\n' + model.architecture + '\n' + model.engine + '\n'
        + model.channel + '\n' + model.version + '\n' + model.artifactUrl + '\n'
        + std::to_string(model.artifactSize) + '\n' + model.sha256;
}

bool VerifyModel(const GuardianClientModel& model, const std::filesystem::path& path,
                 std::string& error) {
    std::error_code fileError;
    if (std::filesystem::file_size(path, fileError) != model.artifactSize || fileError) {
        error = "Guardian model boyutu dogrulanamadi";
        return false;
    }
    if (Sha256File(path) != model.sha256) {
        error = "Guardian model SHA-256 dogrulamasi basarisiz";
        return false;
    }
    std::vector<std::uint8_t> publicKey;
    std::vector<std::uint8_t> signature;
    if (!Base64(SONALIS_RELEASE_PUBLIC_KEY_BASE64, publicKey) || publicKey.size() != 32U
        || !Base64(model.signatureBase64, signature) || signature.size() != 64U) {
        error = "Guardian release anahtari gecersiz";
        return false;
    }
    const std::string canonical = Canonical(model);
    if (crypto_ed25519_check(signature.data(), publicKey.data(),
                             reinterpret_cast<const std::uint8_t*>(canonical.data()),
                             canonical.size()) != 0) {
        error = "Guardian model imzasi gecersiz";
        return false;
    }
    return true;
}

#if defined(SONALIS_REQUIRE_AUTHENTICODE)
bool PinnedSigner(const std::filesystem::path& path) noexcept {
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0;
    DWORD content = 0;
    DWORD format = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format,
                          &store, &message, nullptr)) return false;
    bool matched = false;
    DWORD signerBytes = 0;
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerBytes)
        && signerBytes > 0) {
        std::vector<std::uint8_t> storage(signerBytes);
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, storage.data(), &signerBytes)) {
            const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(storage.data());
            CERT_INFO information{};
            information.Issuer = signer->Issuer;
            information.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT certificate = CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &information, nullptr);
            if (certificate != nullptr) {
                std::array<std::uint8_t, 32> digest{};
                DWORD digestBytes = static_cast<DWORD>(digest.size());
                if (CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
                                          certificate->pbCertEncoded, certificate->cbCertEncoded,
                                          digest.data(), &digestBytes)
                    && digestBytes == static_cast<DWORD>(digest.size())) {
                    constexpr std::string_view expected = SONALIS_AUTHENTICODE_CERT_SHA256;
                    auto nibble = [](const char character) noexcept -> int {
                        if (character >= '0' && character <= '9') return character - '0';
                        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                        return -1;
                    };
                    matched = expected.size() == digest.size() * 2U;
                    for (std::size_t index = 0; matched && index < digest.size(); ++index) {
                        const int high = nibble(expected[index * 2U]);
                        const int low = nibble(expected[index * 2U + 1U]);
                        matched = high >= 0 && low >= 0
                            && digest[index] == static_cast<std::uint8_t>((high << 4) | low);
                    }
                }
                CertFreeCertificateContext(certificate);
            }
        }
    }
    if (message != nullptr) CryptMsgClose(message);
    if (store != nullptr) CertCloseStore(store, 0);
    return matched;
}

bool VerifyScannerAuthenticode(const std::filesystem::path& path) noexcept {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust);
    return result == ERROR_SUCCESS && PinnedSigner(path);
}
#endif

std::filesystem::path ModuleDirectory() {
    std::array<wchar_t, 32'768> value{};
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) return {};
    return std::filesystem::path(std::wstring_view(value.data(), length)).parent_path();
}

std::wstring Quote(const std::wstring& value) {
    std::wstring result{L"\""};
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2U + 1U, L'\\');
            result.push_back(L'"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

bool RunScanner(const std::filesystem::path& scanner, const std::filesystem::path& model,
                const std::filesystem::path& image, const bool preferGpu,
                nlohmann::json& result, std::string& error) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 64U * 1024U)
        || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        error = "Guardian sonuc kanali olusturulamadi";
        return false;
    }
    const std::wstring command = Quote(scanner.wstring()) + L" --model " + Quote(model.wstring())
        + L" --image " + Quote(image.wstring()) + L" --device " + (preferGpu ? L"gpu" : L"cpu");
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(scanner.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                        scanner.parent_path().c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        error = "Guardian yerel tarayici baslatilamadi";
        return false;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS
            | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = 512U * 1024U * 1024U;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 20'000);
    if (wait != WAIT_OBJECT_0) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(process.hProcess, 1);
        error = "Guardian taramasi zaman asimina ugradi";
    }
    std::string output;
    std::array<char, 4096> buffer{};
    DWORD bytes = 0;
    while (output.size() < 64U * 1024U
           && ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)
           && bytes > 0) {
        output.append(buffer.data(), bytes);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
    if (wait != WAIT_OBJECT_0) return false;
    try {
        result = nlohmann::json::parse(output);
        if (exitCode != 0 || !result.value("ok", false)) {
            error = result.value("error", "Guardian taramasi basarisiz");
            return false;
        }
        return true;
    } catch (...) {
        error = "Guardian sonucu gecersiz";
        return false;
    }
}

bool IsAdultLabel(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find("adult") != std::string::npos || value.find("nsfw") != std::string::npos
        || value.find("porn") != std::string::npos || value.find("hentai") != std::string::npos
        || value.find("sexual") != std::string::npos || value.find("explicit") != std::string::npos;
}

}  // namespace

const char* GuardianDecisionName(const GuardianLocalDecision decision) noexcept {
    switch (decision) {
    case GuardianLocalDecision::Safe: return "safe";
    case GuardianLocalDecision::Blocked: return "blocked";
    case GuardianLocalDecision::Critical: return "blocked";
    case GuardianLocalDecision::Review:
    default: return "review";
    }
}

bool GuardianLocalScanner::Scan(PlatformApi& platform, const std::wstring& sourcePath,
                                const std::string& attachmentId,
                                const bool preferGpu, GuardianLocalScanResult& result,
                                std::string& error) const {
    const auto model = platform.LatestGuardianClientModel(SONALIS_UPDATE_CHANNEL, error);
    if (!model) return false;
    const std::filesystem::path root = LocalAppDataPath() / L"Sonalis" / L"guardian";
    const std::filesystem::path models = root / L"models";
    std::error_code directoryError;
    std::filesystem::create_directories(models, directoryError);
    if (directoryError) {
        error = "Guardian model dizini olusturulamadi";
        return false;
    }
    const std::filesystem::path target = models / (Utf8ToWide(model->id + "-" + model->version) + L".onnx");
    if (!std::filesystem::exists(target) || !VerifyModel(*model, target, error)) {
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        std::filesystem::remove(temporary, ignored);
        if (!platform.DownloadGuardianClientModel(*model, temporary.wstring(), error)
            || !VerifyModel(*model, temporary, error)
            || !MoveFileExW(temporary.c_str(), target.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, ignored);
            if (error.empty()) error = "Guardian modeli atomik kaydedilemedi";
            return false;
        }
    }
    const std::filesystem::path scanner = ModuleDirectory() / L"SonalisGuardianScanner.exe";
    if (!std::filesystem::is_regular_file(scanner)) {
        error = "Guardian yerel tarayici kurulu degil";
        return false;
    }
#if defined(SONALIS_REQUIRE_AUTHENTICODE)
    if (!VerifyScannerAuthenticode(scanner)) {
        error = "Guardian yerel tarayici imzasi gecersiz";
        return false;
    }
#endif
    nlohmann::json evaluation;
    const bool useGpu = preferGpu && model->engine == "onnx_directml";
    if (!RunScanner(scanner, target, sourcePath, useGpu, evaluation, error)) return false;
    if (!evaluation.contains("scores") || !evaluation["scores"].is_array()
        || evaluation["scores"].size() != model->labelMap.size()) {
        error = "Guardian model cikti etiketi uyusmuyor";
        return false;
    }
    result = {};
    result.modelId = model->id;
    result.modelVersion = model->version;
    result.durationMs = evaluation.value("durationMs", std::uint64_t{});
    result.scores.reserve(evaluation["scores"].size());
    for (std::size_t index = 0; index < evaluation["scores"].size(); ++index) {
        const float score = evaluation["scores"][index].get<float>();
        if (!std::isfinite(score) || score < -0.001F || score > 1.001F) {
            error = "Guardian model skoru guvenli aralikta degil";
            return false;
        }
        result.scores.push_back(std::clamp(score, 0.0F, 1.0F));
        result.maximumScore = std::max(result.maximumScore, result.scores.back());
        const auto label = model->labelMap.find(std::to_string(index));
        if (label != model->labelMap.end() && IsAdultLabel(label->second)) {
            result.adultScore = std::max(result.adultScore, result.scores.back());
        }
    }
    if (result.adultScore >= model->criticalThreshold) result.decision = GuardianLocalDecision::Critical;
    else if (result.adultScore >= model->rejectThreshold) result.decision = GuardianLocalDecision::Blocked;
    else if (result.adultScore >= model->reviewThreshold) result.decision = GuardianLocalDecision::Review;
    else result.decision = GuardianLocalDecision::Safe;
    std::ostringstream attestation;
    attestation << "sonalis-guardian-local-result-v1\n" << attachmentId << '\n'
                << model->id << '\n' << model->version << '\n'
                << static_cast<int>(result.decision) << '\n' << std::setprecision(8)
                << result.adultScore;
    for (const float score : result.scores) attestation << '\n' << std::setprecision(8) << score;
    result.digest = Sha256Text(attestation.str());
    if (result.digest.size() != 64U) {
        error = "Guardian sonuc ozeti olusturulamadi";
        return false;
    }
    return true;
}

}  // namespace ss
