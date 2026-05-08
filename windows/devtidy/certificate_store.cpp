#include "certificate_store.h"

#include <memory>

#include <wincrypt.h>

namespace devtidy {

namespace {

std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)>
CreateCertificateContext(const CertificateSpec& spec) {
    PCCERT_CONTEXT context = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        spec.encoded.data(),
        static_cast<DWORD>(spec.encoded.size()));
    return std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)>(
        context,
        CertFreeCertificateContext);
}

HCERTSTORE OpenLocalMachineStore(const wchar_t* storeName) {
    return CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE,
        storeName);
}

bool StoreContainsCertificate(HCERTSTORE store, PCCERT_CONTEXT certificate) {
    PCCERT_CONTEXT existing = CertFindCertificateInStore(
        store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_FIND_EXISTING,
        certificate,
        nullptr);
    if (existing != nullptr) {
        CertFreeCertificateContext(existing);
        return true;
    }

    return false;
}

bool EnsureCertificateInStore(
    const CertificateSpec& spec,
    const wchar_t* storeName,
    bool* added,
    std::wstring* errorMessage
) {
    std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> context =
        CreateCertificateContext(spec);
    if (!context) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
        }
        return false;
    }

    HCERTSTORE store = OpenLocalMachineStore(storeName);
    if (store == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
        }
        return false;
    }

    const bool alreadyPresent = StoreContainsCertificate(store, context.get());
    if (!alreadyPresent) {
        if (!CertAddCertificateContextToStore(
                store,
                context.get(),
                CERT_STORE_ADD_REPLACE_EXISTING,
                nullptr)) {
            if (errorMessage != nullptr) {
                *errorMessage = GetLastErrorMessage(GetLastError());
            }
            CertCloseStore(store, 0);
            return false;
        }
    }

    if (added != nullptr) {
        *added = !alreadyPresent;
    }

    CertCloseStore(store, 0);
    return true;
}

bool RemoveCertificateFromStore(
    const CertificateSpec& spec,
    const wchar_t* storeName,
    bool* removed,
    std::wstring* errorMessage
) {
    std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> context =
        CreateCertificateContext(spec);
    if (!context) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
        }
        return false;
    }

    HCERTSTORE store = OpenLocalMachineStore(storeName);
    if (store == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
        }
        return false;
    }

    bool anyRemoved = false;
    for (;;) {
        PCCERT_CONTEXT existing = CertFindCertificateInStore(
            store,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            0,
            CERT_FIND_EXISTING,
            context.get(),
            nullptr);
        if (existing == nullptr) {
            break;
        }

        if (!CertDeleteCertificateFromStore(existing)) {
            if (errorMessage != nullptr) {
                *errorMessage = GetLastErrorMessage(GetLastError());
            }
            CertCloseStore(store, 0);
            return false;
        }

        anyRemoved = true;
    }

    if (removed != nullptr) {
        *removed = anyRemoved;
    }

    CertCloseStore(store, 0);
    return true;
}

}  // namespace

bool InstallCertificates(const std::vector<CertificateSpec>& certificates) {
    bool ok = true;

    for (const CertificateSpec& certificate : certificates) {
        bool rootAdded = false;
        bool publisherAdded = false;
        std::wstring rootError;
        std::wstring publisherError;

        if (!EnsureCertificateInStore(certificate, L"ROOT", &rootAdded, &rootError)) {
            WriteErrorEvent(
                "certificate_install_failed",
                JsonObject({
                    {"certificate_path", JsonText(certificate.path.native())},
                    {"subject", JsonText(certificate.subject)},
                    {"thumbprint", JsonText(certificate.thumbprint)},
                    {"store", JsonText("Root")},
                    {"error", JsonText(rootError)}
                }));
            ok = false;
            continue;
        }

        if (!EnsureCertificateInStore(
                certificate,
                L"TrustedPublisher",
                &publisherAdded,
                &publisherError)) {
            WriteErrorEvent(
                "certificate_install_failed",
                JsonObject({
                    {"certificate_path", JsonText(certificate.path.native())},
                    {"subject", JsonText(certificate.subject)},
                    {"thumbprint", JsonText(certificate.thumbprint)},
                    {"store", JsonText("TrustedPublisher")},
                    {"error", JsonText(publisherError)}
                }));
            ok = false;
            continue;
        }

        WriteInfoEvent(
            (rootAdded || publisherAdded) ? "certificate_installed" : "certificate_present",
            JsonObject({
                {"certificate_path", JsonText(certificate.path.native())},
                {"subject", JsonText(certificate.subject)},
                {"thumbprint", JsonText(certificate.thumbprint)},
                {"root_store_added", Json::Value(rootAdded)},
                {"trusted_publisher_store_added", Json::Value(publisherAdded)}
            }));
    }

    return ok;
}

bool RemoveCertificates(const std::vector<CertificateSpec>& certificates) {
    bool ok = true;

    for (const CertificateSpec& certificate : certificates) {
        bool rootRemoved = false;
        bool publisherRemoved = false;
        std::wstring rootError;
        std::wstring publisherError;

        if (!RemoveCertificateFromStore(certificate, L"ROOT", &rootRemoved, &rootError)) {
            WriteErrorEvent(
                "certificate_remove_failed",
                JsonObject({
                    {"certificate_path", JsonText(certificate.path.native())},
                    {"subject", JsonText(certificate.subject)},
                    {"thumbprint", JsonText(certificate.thumbprint)},
                    {"store", JsonText("Root")},
                    {"error", JsonText(rootError)}
                }));
            ok = false;
        }

        if (!RemoveCertificateFromStore(
                certificate,
                L"TrustedPublisher",
                &publisherRemoved,
                &publisherError)) {
            WriteErrorEvent(
                "certificate_remove_failed",
                JsonObject({
                    {"certificate_path", JsonText(certificate.path.native())},
                    {"subject", JsonText(certificate.subject)},
                    {"thumbprint", JsonText(certificate.thumbprint)},
                    {"store", JsonText("TrustedPublisher")},
                    {"error", JsonText(publisherError)}
                }));
            ok = false;
        }

        if (!rootRemoved && !publisherRemoved) {
            WriteInfoEvent(
                "certificate_absent",
                JsonObject({
                    {"certificate_path", JsonText(certificate.path.native())},
                    {"subject", JsonText(certificate.subject)},
                    {"thumbprint", JsonText(certificate.thumbprint)}
                }));
        } else {
            WriteInfoEvent(
                "certificate_removed",
                JsonObject({
                    {"certificate_path", JsonText(certificate.path.native())},
                    {"subject", JsonText(certificate.subject)},
                    {"thumbprint", JsonText(certificate.thumbprint)},
                    {"root_store_removed", Json::Value(rootRemoved)},
                    {"trusted_publisher_store_removed", Json::Value(publisherRemoved)}
                }));
        }
    }

    return ok;
}

}  // namespace devtidy
