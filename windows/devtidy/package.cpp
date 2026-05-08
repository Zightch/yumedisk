#include "package.h"

#include <algorithm>
#include <fstream>
#include <memory>

#include <wincrypt.h>

namespace devtidy {

namespace {

bool TryFindSinglePackageFile(
    const DeviceSpec& spec,
    const fs::path& packageRoot,
    const std::wstring& extension,
    const char* notFoundEvent,
    const char* multipleFoundEvent,
    const char* fileKind,
    bool allowMissing,
    bool* found,
    fs::path* filePath
) {
    const fs::path packageDir = packageRoot / spec.packageDirName;
    std::error_code ec;

    if (!fs::exists(packageDir, ec) || !fs::is_directory(packageDir, ec)) {
        WriteErrorDeviceEvent(
            "package_dir_not_found",
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())}
            }));
        return false;
    }

    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(packageDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (EqualInsensitive(entry.path().extension().native(), extension)) {
            files.push_back(entry.path());
        }
    }

    if (ec) {
        WriteErrorDeviceEvent(
            "package_dir_scan_failed",
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())},
                {"error", JsonText(ec.message())}
            }));
        return false;
    }

    std::sort(files.begin(), files.end());
    if (files.empty()) {
        if (found != nullptr) {
            *found = false;
        }
        if (allowMissing) {
            return true;
        }

        WriteErrorDeviceEvent(
            notFoundEvent,
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())}
            }));
        return false;
    }
    if (files.size() > 1) {
        Json::Value foundFiles(Json::arrayValue);
        for (const fs::path& candidate : files) {
            foundFiles.append(WideToUtf8(candidate.filename().native()));
        }

        WriteErrorDeviceEvent(
            multipleFoundEvent,
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())},
                {"files", foundFiles},
                {"kind", JsonText(fileKind)}
            }));
        return false;
    }

    if (found != nullptr) {
        *found = true;
    }
    *filePath = files.front();
    return true;
}

bool TryFindOptionalSingleCertificatePath(
    const DeviceSpec& spec,
    const fs::path& packageRoot,
    bool* found,
    fs::path* certificatePath
) {
    return TryFindSinglePackageFile(
        spec,
        packageRoot,
        L".cer",
        "certificate_not_found",
        "multiple_certificate_found",
        "certificate",
        true,
        found,
        certificatePath);
}

bool ReadFileBinary(
    const fs::path& path,
    std::vector<BYTE>* encoded,
    std::wstring* errorMessage
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (errorMessage != nullptr) {
            *errorMessage = L"unable to open file";
        }
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = L"certificate file is empty";
        }
        return false;
    }

    input.seekg(0, std::ios::beg);
    encoded->resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(encoded->data()), static_cast<std::streamsize>(size));
    if (!input) {
        if (errorMessage != nullptr) {
            *errorMessage = L"unable to read file";
        }
        return false;
    }

    return true;
}

bool LoadCertificateSpec(
    const fs::path& path,
    CertificateSpec* spec,
    std::wstring* errorMessage
) {
    std::vector<BYTE> encoded;
    if (!ReadFileBinary(path, &encoded, errorMessage)) {
        return false;
    }

    PCCERT_CONTEXT context = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        encoded.data(),
        static_cast<DWORD>(encoded.size()));
    if (context == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
        }
        return false;
    }

    DWORD hashSize = 0;
    if (!CertGetCertificateContextProperty(context, CERT_HASH_PROP_ID, nullptr, &hashSize) ||
        hashSize == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
        }
        CertFreeCertificateContext(context);
        return false;
    }

    std::vector<BYTE> hash(hashSize);
    if (!CertGetCertificateContextProperty(context, CERT_HASH_PROP_ID, hash.data(), &hashSize)) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
        }
        CertFreeCertificateContext(context);
        return false;
    }

    const DWORD subjectLength = CertGetNameStringW(
        context,
        CERT_NAME_SIMPLE_DISPLAY_TYPE,
        0,
        nullptr,
        nullptr,
        0);
    std::wstring subject;
    if (subjectLength > 1) {
        subject.resize(subjectLength, L'\0');
        CertGetNameStringW(
            context,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0,
            nullptr,
            subject.data(),
            subjectLength);
        subject.resize(subjectLength - 1);
    }

    spec->path = path;
    spec->encoded = std::move(encoded);
    spec->subject = subject;
    spec->thumbprint = BytesToHex(hash.data(), hash.size());

    CertFreeCertificateContext(context);
    return true;
}

}  // namespace

bool TryFindSingleInfPath(
    const DeviceSpec& spec,
    const fs::path& packageRoot,
    fs::path* infPath
) {
    return TryFindSinglePackageFile(
        spec,
        packageRoot,
        L".inf",
        "inf_not_found",
        "multiple_inf_found",
        "inf",
        false,
        nullptr,
        infPath);
}

bool CollectPackageCertificates(
    const fs::path& packageRoot,
    CertificateMode* mode,
    std::vector<CertificateSpec>* certificates
) {
    bool ok = true;
    size_t packagesWithCertificates = 0;
    size_t packagesWithoutCertificates = 0;
    std::vector<std::wstring> packageDirsWithCertificates;
    std::vector<std::wstring> packageDirsWithoutCertificates;
    certificates->clear();

    for (const DeviceSpec& spec : kDeviceSpecs) {
        fs::path certificatePath;
        bool found = false;
        if (!TryFindOptionalSingleCertificatePath(spec, packageRoot, &found, &certificatePath)) {
            ok = false;
            continue;
        }
        if (!found) {
            ++packagesWithoutCertificates;
            packageDirsWithoutCertificates.push_back(spec.packageDirName);
            continue;
        }

        ++packagesWithCertificates;
        packageDirsWithCertificates.push_back(spec.packageDirName);

        CertificateSpec certificate;
        std::wstring errorMessage;
        if (!LoadCertificateSpec(certificatePath, &certificate, &errorMessage)) {
            WriteErrorDeviceEvent(
                "certificate_load_failed",
                spec.name,
                JsonObject({
                    {"certificate_path", JsonText(certificatePath.native())},
                    {"error", JsonText(errorMessage)}
                }));
            ok = false;
            continue;
        }

        const auto duplicate = std::find_if(
            certificates->begin(),
            certificates->end(),
            [&](const CertificateSpec& existing) {
                return EqualInsensitive(existing.thumbprint, certificate.thumbprint);
            });
        if (duplicate == certificates->end()) {
            certificates->push_back(std::move(certificate));
        }
    }

    if (!ok) {
        return false;
    }

    if (packagesWithCertificates == 0) {
        *mode = CertificateMode::Release;
        WriteInfoEvent(
            "certificate_mode",
            JsonObject({
                {"mode", JsonText("release")},
                {"package_count", JsonSize(kDeviceSpecs.size())},
                {"certificate_count", JsonSize(0)},
                {"package_dirs_with_certificate", JsonTextArray(packageDirsWithCertificates)},
                {"package_dirs_without_certificate", JsonTextArray(packageDirsWithoutCertificates)}
            }));
        return true;
    }

    *mode = (packagesWithoutCertificates == 0)
        ? CertificateMode::SelfSigned
        : CertificateMode::Mixed;
    WriteInfoEvent(
        "certificate_mode",
        JsonObject({
            {"mode", JsonText(
                (*mode == CertificateMode::SelfSigned) ? "self_signed" : "mixed")},
            {"package_count", JsonSize(kDeviceSpecs.size())},
            {"certificate_count", JsonSize(certificates->size())},
            {"package_dirs_with_certificate", JsonTextArray(packageDirsWithCertificates)},
            {"package_dirs_without_certificate", JsonTextArray(packageDirsWithoutCertificates)}
        }));
    return !certificates->empty();
}

}  // namespace devtidy
