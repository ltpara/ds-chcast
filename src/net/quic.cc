// MsQuic transport and session state for chast/protocol.h.
//
// Threading model: MsQuic delivers listener, connection and stream events on
// its own worker threads; this file records them under Impl::mutex. All public
// operations run on the application thread and wait on condition variables.
// The only object shared with the transport besides handles is the receive
// queue (protected) and PendingSend contexts (owned by MsQuic until
// SEND_COMPLETE, then deleted).
//
// The server certificate is a CNG RSA key plus a self-signed certificate in
// the current user's certificate store, created once and reused on later
// runs. Its SHA-256 fingerprint is the server's identity: clients pin it and
// MsQuic's certificate validation callback refuses anything else.

// Winsock 2 must precede windows.h, which chast/quic.h pulls in through
// chast/error.h; otherwise the SDK sees winsock 1 first and ws2tcpip.h fails.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chast/quic.h"

#include <windows.h>
#include <ncrypt.h>
#include <wincrypt.h>

#include <msquic.h>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chast/identity.h"

namespace chast {

namespace {

constexpr size_t kMaxInFlightBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxInFlightSends = 4096;
constexpr uint64_t kKeepAliveMs = 5000;

uint64_t steady_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
constexpr wchar_t kKeyContainer[] = L"chast-quic-key";
// Unique enough that the lookup in the user's store cannot confuse the
// identity with a certificate another application placed there.
constexpr wchar_t kIdentitySubject[] = L"CN=chast-device";
constexpr wchar_t kIdentityCommonName[] = L"chast-device";

// Length-independent comparison for the access token. The lengths are not
// secret (they travel in the Hello payload); the bytes are.
bool tokens_match(const std::string& presented, const std::string& expected) {
    size_t diff = presented.size() ^ expected.size();
    for (size_t i = 0; i < expected.size(); ++i) {
        const unsigned char actual =
            i < presented.size() ? static_cast<unsigned char>(presented[i]) : 0;
        diff |= static_cast<size_t>(actual ^ static_cast<unsigned char>(expected[i]));
    }
    return diff == 0;
}

std::string format_address(const QUIC_ADDR& address) {
    char text[64]{};
    if (address.si_family == AF_INET) {
        if (!inet_ntop(AF_INET, &address.Ipv4.sin_addr, text, sizeof(text))) {
            return {};
        }
        return std::string(text) + ":" + std::to_string(ntohs(address.Ipv4.sin_port));
    }
    if (address.si_family == AF_INET6) {
        if (!inet_ntop(AF_INET6, &address.Ipv6.sin6_addr, text, sizeof(text))) {
            return {};
        }
        return "[" + std::string(text) + "]:" + std::to_string(ntohs(address.Ipv6.sin6_port));
    }
    return {};
}

std::string status_hex(uint32_t status) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08X", status);
    return text;
}

std::string sec_status_hex(SECURITY_STATUS status) {
    return status_hex(static_cast<uint32_t>(status));
}

Error quic_error(QUIC_STATUS status, const char* what) {
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed: " + status_hex(status));
}

struct DataBlob {
    CRYPT_DATA_BLOB blob{};
    ~DataBlob() {
        if (blob.pbData) {
            HeapFree(GetProcessHeap(), 0, blob.pbData);
        }
    }
};

bool encode_object(LPCSTR type, const void* info, DataBlob& out) {
    if (!CryptEncodeObject(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, type, info, nullptr,
                           &out.blob.cbData)) {
        return false;
    }
    out.blob.pbData =
        static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, out.blob.cbData));
    if (!out.blob.pbData) {
        return false;
    }
    return CryptEncodeObject(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, type, info,
                             out.blob.pbData, &out.blob.cbData) != FALSE;
}

std::string certificate_fingerprint(PCCERT_CONTEXT certificate) {
    if (!certificate || !certificate->pbCertEncoded || certificate->cbCertEncoded == 0) {
        return {};
    }
    BYTE hash[64]{};
    DWORD hash_size = sizeof(hash);
    if (!CryptHashCertificate(0, CALG_SHA_256, 0, certificate->pbCertEncoded,
                              certificate->cbCertEncoded, hash, &hash_size) ||
        hash_size != 32) {
        return {};
    }
    static const char kHex[] = "0123456789ABCDEF";
    std::string text;
    text.reserve(hash_size * 2);
    for (DWORD i = 0; i < hash_size; ++i) {
        text.push_back(kHex[hash[i] >> 4]);
        text.push_back(kHex[hash[i] & 0x0F]);
    }
    return text;
}

// True when the certificate's private key is the fixed chast container. Any
// certificate in the user's store could share the subject, so the key is what
// identifies ours.
bool certificate_uses_our_key(PCCERT_CONTEXT certificate) {
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID, nullptr,
                                           &size) ||
        size == 0) {
        return false;
    }
    std::vector<BYTE> buffer(size);
    if (!CertGetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID,
                                           buffer.data(), &size)) {
        return false;
    }
    const auto* info = reinterpret_cast<const CRYPT_KEY_PROV_INFO*>(buffer.data());
    return info->pwszContainerName != nullptr &&
           wcscmp(info->pwszContainerName, kKeyContainer) == 0;
}

bool certificate_is_current(PCCERT_CONTEXT certificate) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return CompareFileTime(&certificate->pCertInfo->NotBefore, &now) <= 0 &&
           CompareFileTime(&now, &certificate->pCertInfo->NotAfter) < 0;
}

// Newest valid certificate in `store` that belongs to this installation.
PCCERT_CONTEXT find_stored_identity(HCERTSTORE store) {
    PCCERT_CONTEXT best = nullptr;
    FILETIME best_time{};
    for (PCCERT_CONTEXT current = CertEnumCertificatesInStore(store, nullptr); current;
         current = CertEnumCertificatesInStore(store, current)) {
        wchar_t name[256]{};
        if (!CertGetNameStringW(current, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                256) ||
            wcscmp(name, kIdentityCommonName) != 0) {
            continue;
        }
        if (!certificate_uses_our_key(current) || !certificate_is_current(current)) {
            continue;
        }
        if (!best || CompareFileTime(&current->pCertInfo->NotBefore, &best_time) > 0) {
            if (best) {
                CertFreeCertificateContext(best);
            }
            best = CertDuplicateCertificateContext(current);
            best_time = current->pCertInfo->NotBefore;
        }
    }
    return best;
}

// Creates the server credential. The RSA key lives in the selected CNG key
// store under a fixed container name so restarts reuse it; the certificate
// itself is stored by open_or_create_identity().
PCCERT_CONTEXT create_identity_certificate(IdentityScope scope, std::string* error) {
    const DWORD key_flags =
        NCRYPT_SILENT_FLAG |
        (scope == IdentityScope::LocalMachine ? NCRYPT_MACHINE_KEY_FLAG : 0);
    NCRYPT_PROV_HANDLE provider = 0;
    SECURITY_STATUS sec = NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0);
    if (FAILED(sec)) {
        *error = "NCryptOpenStorageProvider failed: " + sec_status_hex(sec);
        return nullptr;
    }
    NCRYPT_KEY_HANDLE key = 0;
    sec = NCryptOpenKey(provider, &key, kKeyContainer, 0, key_flags);
    if (sec == NTE_BAD_KEYSET) {
        sec = NCryptCreatePersistedKey(provider, &key, NCRYPT_RSA_ALGORITHM, kKeyContainer, 0,
                                       key_flags);
        if (!FAILED(sec)) {
            DWORD key_size = 2048;
            DWORD usage = NCRYPT_ALLOW_SIGNING_FLAG;
            DWORD export_policy = NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
            sec = NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY,
                                    reinterpret_cast<PBYTE>(&key_size), sizeof(key_size), 0);
            if (!FAILED(sec)) {
                sec = NCryptSetProperty(key, NCRYPT_KEY_USAGE_PROPERTY,
                                        reinterpret_cast<PBYTE>(&usage), sizeof(usage), 0);
            }
            if (!FAILED(sec)) {
                sec = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY,
                                        reinterpret_cast<PBYTE>(&export_policy),
                                        sizeof(export_policy), 0);
            }
            if (!FAILED(sec)) {
                sec = NCryptFinalizeKey(key, 0);
            }
        }
    }
    if (FAILED(sec) || !key) {
        *error = "cannot create the QUIC server key: " + sec_status_hex(sec);
        if (key) {
            NCryptFreeObject(key);
        }
        NCryptFreeObject(provider);
        return nullptr;
    }

    DWORD subject_size = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, kIdentitySubject, CERT_X500_NAME_STR, nullptr,
                        nullptr, &subject_size, nullptr)) {
        *error = "CertStrToNameW failed";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }
    std::vector<BYTE> subject(subject_size);
    if (!CertStrToNameW(X509_ASN_ENCODING, kIdentitySubject, CERT_X500_NAME_STR, nullptr,
                        subject.data(), &subject_size, nullptr)) {
        *error = "CertStrToNameW failed";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }
    CERT_NAME_BLOB subject_blob{subject_size, subject.data()};

    CERT_ALT_NAME_ENTRY alt_name{};
    alt_name.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    alt_name.pwszDNSName = const_cast<LPWSTR>(L"localhost");
    CERT_ALT_NAME_INFO alt_info{1, &alt_name};
    DataBlob san;
    if (!encode_object(szOID_SUBJECT_ALT_NAME, &alt_info, san)) {
        *error = "cannot encode the subject alternative name";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }

    LPSTR server_auth = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
    CERT_ENHKEY_USAGE usage_info{1, &server_auth};
    DataBlob eku;
    if (!encode_object(X509_ENHANCED_KEY_USAGE, &usage_info, eku)) {
        *error = "cannot encode the extended key usage";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }

    BYTE key_usage = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
    CRYPT_BIT_BLOB key_usage_info{1, &key_usage, 0};
    DataBlob key_usage_encoded;
    if (!encode_object(X509_KEY_USAGE, &key_usage_info, key_usage_encoded)) {
        *error = "cannot encode the key usage";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }

    CERT_EXTENSION extensions[3]{};
    extensions[0].pszObjId = const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME);
    extensions[0].Value = san.blob;
    extensions[1].pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
    extensions[1].Value = eku.blob;
    extensions[2].pszObjId = const_cast<LPSTR>(szOID_KEY_USAGE);
    extensions[2].Value = key_usage_encoded.blob;
    CERT_EXTENSIONS extension_list{3, extensions};

    CRYPT_KEY_PROV_INFO key_info{};
    key_info.pwszContainerName = const_cast<LPWSTR>(kKeyContainer);
    key_info.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
    key_info.dwProvType = 0;
    // The certificate is read back from LocalMachine\My by a different
    // process than the one that created it, so the stored provider info has to
    // name the machine key store explicitly.
    key_info.dwFlags =
        NCRYPT_SILENT_FLAG |
        (scope == IdentityScope::LocalMachine ? static_cast<DWORD>(CRYPT_MACHINE_KEYSET) : 0);
    key_info.dwKeySpec = AT_KEYEXCHANGE;

    CRYPT_ALGORITHM_IDENTIFIER signature{};
    signature.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    SYSTEMTIME not_before{};
    SYSTEMTIME not_after{};
    GetSystemTime(&not_before);
    FILETIME expiry_filetime{};
    if (!SystemTimeToFileTime(&not_before, &expiry_filetime)) {
        *error = "SystemTimeToFileTime failed";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }
    ULARGE_INTEGER expiry{};
    expiry.LowPart = expiry_filetime.dwLowDateTime;
    expiry.HighPart = expiry_filetime.dwHighDateTime;
    expiry.QuadPart += 5ull * 365ull * 24ull * 60ull * 60ull * 10000000ull;
    expiry_filetime.dwLowDateTime = expiry.LowPart;
    expiry_filetime.dwHighDateTime = expiry.HighPart;
    FileTimeToSystemTime(&expiry_filetime, &not_after);

    PCCERT_CONTEXT certificate = CertCreateSelfSignCertificate(
        static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(key), &subject_blob, 0, &key_info,
        &signature, &not_before, &not_after, &extension_list);
    if (!certificate) {
        *error = "CertCreateSelfSignCertificate failed, GetLastError=" +
                 std::to_string(GetLastError());
    }
    NCryptFreeObject(key);
    NCryptFreeObject(provider);
    return certificate;
}

// The identity must survive restarts, otherwise a pinned client would refuse
// the server after a reboot. Reuse the stored certificate when it is still
// valid; create and store one otherwise.
PCCERT_CONTEXT open_or_create_identity(IdentityScope scope, std::string* error) {
    const DWORD store_location = scope == IdentityScope::LocalMachine
                                     ? CERT_SYSTEM_STORE_LOCAL_MACHINE
                                     : CERT_SYSTEM_STORE_CURRENT_USER;
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, store_location, L"MY");
    if (!store) {
        *error = std::string("cannot open the ") +
                 (scope == IdentityScope::LocalMachine ? "machine" : "current user's") +
                 " certificate store, GetLastError=" + std::to_string(GetLastError());
        return nullptr;
    }
    PCCERT_CONTEXT existing = find_stored_identity(store);
    if (existing) {
        CertCloseStore(store, 0);
        return existing;
    }
    PCCERT_CONTEXT created = create_identity_certificate(scope, error);
    if (!created) {
        CertCloseStore(store, 0);
        return nullptr;
    }
    if (!CertAddCertificateContextToStore(store, created, CERT_STORE_ADD_REPLACE_EXISTING,
                                          nullptr)) {
        // Without the stored certificate the fingerprint changes on the next
        // run, which silently breaks every pin. Refuse rather than pretend the
        // identity is stable.
        *error = "cannot store the server identity, GetLastError=" +
                 std::to_string(GetLastError());
        CertFreeCertificateContext(created);
        CertCloseStore(store, 0);
        return nullptr;
    }
    CertCloseStore(store, 0);
    return created;
}

}  // namespace

struct QuicSession::Impl {
    enum class Role { Server, Client };

    struct PendingSend {
        std::vector<uint8_t> bytes;
        QUIC_BUFFER buffer{};
    };

    Role role = Role::Server;
    QuicServerConfig server_config;
    QuicClientConfig client_config;
    // Server: fingerprint of the certificate it presents. Client: fingerprint
    // of the certificate the peer presented, and the pin it was checked
    // against. Both canonical uppercase hex.
    std::string identity_fingerprint;
    std::string peer_fingerprint;
    std::string pinned_fingerprint;
    bool allow_any_certificate = false;

    const QUIC_API_TABLE* api = nullptr;
    HQUIC registration = nullptr;
    HQUIC configuration = nullptr;
    HQUIC listener = nullptr;
    PCCERT_CONTEXT certificate = nullptr;
    std::atomic<HQUIC> connection{nullptr};
    std::atomic<HQUIC> control{nullptr};
    std::atomic<HQUIC> video{nullptr};
    std::atomic<HQUIC> audio{nullptr};

    mutable std::mutex mutex;
    std::condition_variable cv;
    bool connected = false;
    bool shutdown_complete = false;
    bool peer_closed = false;
    bool hello_ready = false;
    bool hello_ack_ready = false;
    bool video_finished = false;
    bool control_finished = false;
    bool audio_finished = false;
    bool video_shutdown_complete = false;
    bool control_shutdown_complete = false;
    bool audio_shutdown_complete = false;
    bool audio_stopped = false;
    bool stats_ready = false;
    bool bye_ready = false;
    bool keyframe_requested = false;
    bool announced = false;
    bool transport_failed = false;
    Status transport_status = Status::Ok;
    std::string transport_message;
    ServerInfo peer_info;
    ClientStats peer_stats;
    // Client side: a StreamInfo arrived since take_stream_update().
    bool stream_update_ready = false;
    uint64_t stream_updates = 0;
    // Server side: what Hello asked to have captured.
    uint16_t requested_display = kDisplayAuto;
    std::string requested_peer;
    // Server: a Hello the token check refused, waiting for the application
    // thread to answer it. Delivery happens there because closing a
    // connection from inside its own callback is not allowed.
    bool pending_denial = false;
    AccessDenial pending_code = AccessDenial::InvalidToken;
    std::string pending_reason;
    // Server: the one-time token has been spent.
    bool token_consumed = false;
    // Client: the server refused the Hello, with its reason.
    bool access_denied_ready = false;
    std::string denial_reason;
    std::optional<AccessDenial> denial_code;
    std::deque<std::vector<uint8_t>> received;
    std::deque<std::vector<uint8_t>> audio_received;
    ControlParser control_parser;
    FrameParser frame_parser;
    AudioParser audio_parser;
    // Client: server-initiated streams whose first bytes have not arrived yet.
    // The packet magic, not the stream order, says which one is which.
    std::vector<std::pair<HQUIC, std::vector<uint8_t>>> unclassified;
    // Server only, when QuicServerConfig::allow_input is set.
    std::unique_ptr<InputInjector> injector;
    std::deque<InputEvent> input_events;
    size_t input_releases = 0;
    std::deque<ClipboardUpdate> clipboard_updates;
    uint64_t clipboard_received = 0;
    // Client: the newest pointer state. A viewer that falls behind wants the
    // current pointer, not a replay, so a newer update replaces an older one
    // that was never taken.
    std::optional<CursorUpdate> cursor_update;
    uint64_t cursor_received = 0;
    // Client: last clock-sync result. `last_reply_probe_us` rejects a reply
    // that overtook a newer one on the control stream.
    LatencySample latency;
    uint64_t last_reply_probe_us = 0;

    std::atomic<uint64_t> next_seq{0};
    std::atomic<uint64_t> next_audio_seq{0};
    std::atomic<bool> transport_down{false};

    std::mutex send_mutex;
    std::condition_variable send_cv;
    size_t in_flight_bytes = 0;
    size_t in_flight_sends = 0;

    ~Impl() { close(); }

    Error fail(Status status, const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!transport_failed) {
                transport_failed = true;
                transport_status = status;
                transport_message = message;
            }
        }
        transport_down = true;
        cv.notify_all();
        send_cv.notify_all();
        return Error(status, message);
    }

    void note_transport_failure(const std::string& message) {
        fail(Status::Unavailable, message);
    }

    Error recorded_error() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (!transport_failed) {
            return Error::success();
        }
        return Error(transport_status, transport_message);
    }

    bool failed() const {
        std::lock_guard<std::mutex> lock(mutex);
        return transport_failed;
    }

    // ----- callbacks -----

    static QUIC_STATUS QUIC_API listener_callback(HQUIC listener_handle, void* context,
                                                  QUIC_LISTENER_EVENT* event) {
        (void)listener_handle;
        return static_cast<Impl*>(context)->on_listener(*event);
    }

    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection_handle, void* context,
                                                    QUIC_CONNECTION_EVENT* event) {
        (void)connection_handle;
        return static_cast<Impl*>(context)->on_connection(*event);
    }

    static QUIC_STATUS QUIC_API stream_callback(HQUIC stream, void* context,
                                                QUIC_STREAM_EVENT* event) {
        return static_cast<Impl*>(context)->on_stream(stream, *event);
    }

    QUIC_STATUS on_listener(QUIC_LISTENER_EVENT& event) {
        if (event.Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
            return QUIC_STATUS_SUCCESS;
        }
        HQUIC expected = nullptr;
        if (!connection.compare_exchange_strong(expected, event.NEW_CONNECTION.Connection)) {
            // The tool serves one viewer at a time; a second connection is
            // refused before the handshake continues. Shutting it down first
            // tells that client right away instead of leaving it retransmit.
            api->ConnectionShutdown(event.NEW_CONNECTION.Connection,
                                    QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(event.NEW_CONNECTION.Connection);
            return QUIC_STATUS_SUCCESS;
        }
        api->SetCallbackHandler(event.NEW_CONNECTION.Connection,
                                reinterpret_cast<void*>(&Impl::connection_callback), this);
        const QUIC_STATUS status = api->ConnectionSetConfiguration(
            event.NEW_CONNECTION.Connection, configuration);
        if (QUIC_FAILED(status)) {
            fail(Status::Unavailable,
                 "ConnectionSetConfiguration failed: " + status_hex(status));
            api->ConnectionClose(event.NEW_CONNECTION.Connection);
            connection = nullptr;
            return status;
        }
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS on_connection(QUIC_CONNECTION_EVENT& event) {
        switch (event.Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    connected = true;
                }
                cv.notify_all();
                break;
            }
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                HQUIC stream = event.PEER_STREAM_STARTED.Stream;
                const bool unidirectional =
                    (event.PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0;
                api->SetCallbackHandler(stream,
                                        reinterpret_cast<void*>(&Impl::stream_callback), this);
                bool accepted = false;
                if (role == Role::Server && !unidirectional) {
                    HQUIC expected = nullptr;
                    accepted = control.compare_exchange_strong(expected, stream);
                } else if (role == Role::Client && unidirectional) {
                    // Which media stream this is cannot be told from the start
                    // event: QUIC does not order two server-initiated streams'
                    // first bytes. Park it until a RECEIVE names its magic.
                    std::lock_guard<std::mutex> lock(mutex);
                    bool known = false;
                    for (const auto& entry : unclassified) {
                        known = known || entry.first == stream;
                    }
                    if (!known) {
                        unclassified.emplace_back(stream, std::vector<uint8_t>{});
                    }
                    accepted = !known;
                }
                if (!accepted) {
                    api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                }
                cv.notify_all();
                break;
            }
            case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
                if (role != Role::Client) {
                    break;
                }
                // Schannel hands over a PCCERT_CONTEXT. Validation was
                // deferred so the fingerprint comparison below, not the OS
                // chain, decides whether the peer is the pinned server.
                const auto* presented = reinterpret_cast<const CERT_CONTEXT*>(
                    event.PEER_CERTIFICATE_RECEIVED.Certificate);
                const std::string actual = certificate_fingerprint(presented);
                bool accepted = false;
                std::string reason;
                if (actual.empty()) {
                    reason = "the server presented no readable certificate";
                } else if (!pinned_fingerprint.empty()) {
                    accepted = fingerprint_matches(pinned_fingerprint, actual);
                    if (!accepted) {
                        reason = "the server's certificate " + format_fingerprint(actual) +
                                 " does not match the pinned fingerprint";
                    }
                } else if (allow_any_certificate) {
                    accepted = true;
                } else {
                    reason = "no pinned server fingerprint";
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    peer_fingerprint = actual;
                }
                api->ConnectionCertificateValidationComplete(
                    connection.load(), accepted ? TRUE : FALSE,
                    accepted ? QUIC_TLS_ALERT_CODE_SUCCESS : QUIC_TLS_ALERT_CODE_USER_CANCELED);
                if (!accepted) {
                    note_transport_failure("server certificate: " + reason);
                }
                break;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: {
                if (event.SHUTDOWN_INITIATED_BY_TRANSPORT.Status != QUIC_STATUS_CONNECTION_IDLE) {
                    note_transport_failure("QUIC transport closed: " +
                                           status_hex(event.SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
                }
                break;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    peer_closed = true;
                }
                cv.notify_all();
                break;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    shutdown_complete = true;
                    connected = false;
                }
                transport_down = true;
                cv.notify_all();
                send_cv.notify_all();
                break;
            }
            default:
                break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    static void copy_chunks(std::deque<std::vector<uint8_t>>& queue,
                            const QUIC_STREAM_EVENT& event) {
        for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
            queue.emplace_back(buffer.Buffer, buffer.Buffer + buffer.Length);
        }
    }

    // Buffers a server-initiated stream until its packet magic identifies it
    // as video or audio, then moves those bytes into the matching queue.
    void classify_media_stream(HQUIC stream, QUIC_STREAM_EVENT& event) {
        bool unknown = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<uint8_t>* pending = nullptr;
            for (auto& entry : unclassified) {
                if (entry.first == stream) {
                    pending = &entry.second;
                    break;
                }
            }
            if (!pending) {
                unclassified.emplace_back(stream, std::vector<uint8_t>{});
                pending = &unclassified.back().second;
            }
            for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                pending->insert(pending->end(), buffer.Buffer, buffer.Buffer + buffer.Length);
            }
            if (pending->size() < 4) {
                // Not even the magic is complete; keep waiting.
                return;
            }
            std::deque<std::vector<uint8_t>>* queue = nullptr;
            if (std::memcmp(pending->data(), "CFRM", 4) == 0) {
                HQUIC expected = nullptr;
                if (video.compare_exchange_strong(expected, stream)) {
                    queue = &received;
                }
            } else if (std::memcmp(pending->data(), "CAUD", 4) == 0) {
                HQUIC expected = nullptr;
                if (audio.compare_exchange_strong(expected, stream)) {
                    queue = &audio_received;
                }
            } else {
                unknown = true;
            }
            if (queue) {
                queue->push_back(std::move(*pending));
            } else if (!unknown) {
                // A second stream of a kind that already has its slot.
                unknown = true;
            }
            unclassified.erase(
                std::remove_if(unclassified.begin(), unclassified.end(),
                               [stream](const std::pair<HQUIC, std::vector<uint8_t>>& entry) {
                                   return entry.first == stream;
                               }),
                unclassified.end());
        }
        if (unknown) {
            note_transport_failure("the server opened an unusable media stream");
        }
        cv.notify_all();
    }

    QUIC_STATUS on_stream(HQUIC stream, QUIC_STREAM_EVENT& event) {
        switch (event.Type) {
            case QUIC_STREAM_EVENT_START_COMPLETE:
                if (QUIC_FAILED(event.START_COMPLETE.Status)) {
                    note_transport_failure("QUIC stream start failed: " +
                                           status_hex(event.START_COMPLETE.Status));
                }
                break;
            case QUIC_STREAM_EVENT_RECEIVE: {
                if (stream == video.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    copy_chunks(received, event);
                    cv.notify_all();
                } else if (stream == audio.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    copy_chunks(audio_received, event);
                    cv.notify_all();
                } else if (stream == control.load()) {
                    std::vector<std::pair<ControlType, std::vector<uint8_t>>> messages;
                    std::string error;
                    bool ok = true;
                    for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                        const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                        if (!control_parser.push(buffer.Buffer, buffer.Length, messages, &error)) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) {
                        note_transport_failure("control stream: " + error);
                        break;
                    }
                    for (const auto& message : messages) {
                        handle_control(message.first, message.second);
                    }
                } else {
                    classify_media_stream(stream, event);
                }
                break;
            }
            case QUIC_STREAM_EVENT_SEND_COMPLETE: {
                auto* pending = static_cast<PendingSend*>(event.SEND_COMPLETE.ClientContext);
                if (pending) {
                    release_send(pending->bytes.size());
                    delete pending;
                }
                break;
            }
            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stream == video.load()) {
                        video_finished = true;
                    } else if (stream == audio.load()) {
                        audio_finished = true;
                    } else if (stream == control.load()) {
                        control_finished = true;
                    }
                }
                cv.notify_all();
                break;
            }
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
                if (stream == audio.load()) {
                    // Audio is optional: a broken audio stream ends audio, not
                    // the video session.
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        audio_finished = true;
                    }
                    cv.notify_all();
                    break;
                }
                note_transport_failure("QUIC peer aborted a stream");
                if (stream == video.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    video_finished = true;
                }
                cv.notify_all();
                break;
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stream == video.load()) {
                        video_shutdown_complete = true;
                    } else if (stream == audio.load()) {
                        audio_shutdown_complete = true;
                    } else if (stream == control.load()) {
                        control_shutdown_complete = true;
                    }
                }
                cv.notify_all();
                break;
            }
            default:
                break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    void handle_control(ControlType type, const std::vector<uint8_t>& payload) {
        std::string error;
        switch (type) {
            case ControlType::Hello: {
                if (role != Role::Server) {
                    break;
                }
                uint16_t display = kDisplayAuto;
                std::string token;
                if (!parse_hello(payload, display, token, &error)) {
                    note_transport_failure("Hello: " + error);
                    break;
                }
                // Copied under the lock because set_access_token() may replace
                // the token between sessions.
                std::string expected;
                bool used = false;
                bool reusable = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    expected = server_config.access_token;
                    used = token_consumed;
                    reusable = server_config.reuse_access_token;
                }
                if (!expected.empty()) {
                    if (!tokens_match(token, expected)) {
                        std::lock_guard<std::mutex> lock(mutex);
                        pending_denial = true;
                        pending_code = AccessDenial::InvalidToken;
                        pending_reason = "the access token was not accepted";
                        cv.notify_all();
                        break;
                    }
                    if (used && !reusable) {
                        std::lock_guard<std::mutex> lock(mutex);
                        pending_denial = true;
                        pending_code = AccessDenial::TokenUsed;
                        pending_reason = "the one-time access token was already used";
                        cv.notify_all();
                        break;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    requested_display = display;
                    requested_peer = remote_address();
                    hello_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::AccessDenied: {
                if (role != Role::Client) {
                    break;
                }
                AccessDenial code = AccessDenial::InvalidToken;
                std::string reason;
                if (!parse_access_denied(payload, code, reason, &error)) {
                    note_transport_failure("AccessDenied: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    denial_reason = std::move(reason);
                    denial_code = code;
                    access_denied_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::HelloAck: {
                if (role != Role::Client) {
                    break;
                }
                ServerInfo info;
                if (!parse_hello_ack(payload, info, &error)) {
                    note_transport_failure("HelloAck: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    peer_info = std::move(info);
                    hello_ack_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::StreamInfo: {
                if (role != Role::Client) {
                    break;
                }
                // Same payload as HelloAck. Frames keep flowing on the video
                // stream; this only replaces the description.
                ServerInfo info;
                if (!parse_hello_ack(payload, info, &error)) {
                    note_transport_failure("StreamInfo: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    peer_info = std::move(info);
                    stream_update_ready = true;
                    ++stream_updates;
                }
                cv.notify_all();
                break;
            }
            case ControlType::KeyframeRequest: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    keyframe_requested = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::Stats: {
                ClientStats stats;
                if (!parse_stats(payload, stats, &error)) {
                    note_transport_failure("Stats: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    peer_stats = stats;
                    stats_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::Bye: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    bye_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::InputEvent: {
                if (role != Role::Server || !server_config.allow_input) {
                    // The HelloAck already told the viewer input is off; a
                    // client that sends it anyway is dropped, not fatal.
                    break;
                }
                InputEvent event;
                if (!parse_input_event(payload, event, &error)) {
                    // Same policy as a malformed Stats or Hello: the peer is
                    // not speaking the protocol, so the session ends rather
                    // than dropping events silently.
                    note_transport_failure("InputEvent: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // The observation queue is a convenience for the tools;
                    // it must not grow without bound when nobody drains it.
                    if (input_events.size() >= 4096) {
                        input_events.pop_front();
                    }
                    input_events.push_back(event);
                }
                if (injector) {
                    injector->submit(event);
                }
                break;
            }
            case ControlType::InputRelease: {
                if (role != Role::Server || !server_config.allow_input) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++input_releases;
                }
                if (injector) {
                    injector->release_held();
                }
                break;
            }
            case ControlType::LatencyProbe: {
                if (role != Role::Server) {
                    break;
                }
                uint64_t client_us = 0;
                if (!parse_latency_probe(payload, client_us, &error)) {
                    note_transport_failure("LatencyProbe: " + error);
                    break;
                }
                const uint64_t recv_us = steady_us();
                const std::vector<uint8_t> reply =
                    make_latency_reply(client_us, recv_us, steady_us());
                // A full send queue drops this sample; the viewer probes again
                // in a second. Blocking a MsQuic worker here would be worse.
                try_send_bytes(control.load(), reply, QUIC_SEND_FLAG_NONE);
                break;
            }
            case ControlType::LatencyReply: {
                if (role != Role::Client) {
                    break;
                }
                uint64_t client_us = 0;
                uint64_t host_recv_us = 0;
                uint64_t host_send_us = 0;
                if (!parse_latency_reply(payload, client_us, host_recv_us, host_send_us,
                                         &error)) {
                    note_transport_failure("LatencyReply: " + error);
                    break;
                }
                const uint64_t received_us = steady_us();
                if (received_us < client_us) {
                    // The host echoed a stamp from the viewer's future. Only a
                    // broken clock or a forged message does that.
                    break;
                }
                const uint64_t processing_us = host_send_us - host_recv_us;
                const uint64_t round_trip_us = received_us - client_us;
                if (processing_us > round_trip_us) {
                    break;
                }
                const uint64_t rtt_us = round_trip_us - processing_us;
                const int64_t offset_us =
                    (static_cast<int64_t>(host_recv_us) - static_cast<int64_t>(client_us) +
                     static_cast<int64_t>(host_send_us) - static_cast<int64_t>(received_us)) /
                    2;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!latency.valid || client_us >= last_reply_probe_us) {
                        last_reply_probe_us = client_us;
                        latency.valid = true;
                        latency.rtt_us = rtt_us;
                        latency.offset_us = offset_us;
                    }
                    ++latency.replies;
                }
                cv.notify_all();
                break;
            }
            case ControlType::ClipboardUpdate: {
                if (role == Role::Server && !server_config.allow_clipboard) {
                    // The HelloAck already reported clipboard off; a client
                    // that sends it anyway is dropped, not fatal.
                    break;
                }
                ClipboardUpdate update;
                if (!parse_clipboard_update(payload, update, &error)) {
                    note_transport_failure("ClipboardUpdate: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (clipboard_updates.size() >= 64) {
                        clipboard_updates.pop_front();
                    }
                    clipboard_updates.push_back(std::move(update));
                    ++clipboard_received;
                }
                cv.notify_all();
                break;
            }
            case ControlType::CursorUpdate: {
                if (role != Role::Client) {
                    break;
                }
                CursorUpdate update;
                if (!parse_cursor_update(payload, update, &error)) {
                    // The pointer is cosmetic: a malformed update is skipped
                    // rather than ending a session whose video is fine.
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    cursor_update = std::move(update);
                    ++cursor_received;
                }
                break;
            }
            default:
                break;
        }
    }

    // ----- sending -----

    void release_send(size_t bytes) {
        {
            std::lock_guard<std::mutex> lock(send_mutex);
            --in_flight_sends;
            in_flight_bytes -= bytes;
        }
        send_cv.notify_all();
    }

    // `wait_for_capacity` is false only for messages sent from a MsQuic
    // callback (a latency reply). Blocking there could stall the transport's
    // own worker, so such a send is dropped when the queue is full instead.
    Error send_bytes_impl(HQUIC stream, std::vector<uint8_t> bytes, QUIC_SEND_FLAGS flags,
                          bool wait_for_capacity) {
        if (!stream) {
            return Error(Status::Unavailable, "no stream is open");
        }
        if (bytes.empty()) {
            return Error(Status::InvalidCall, "refusing to send an empty message");
        }
        {
            std::unique_lock<std::mutex> lock(send_mutex);
            if (wait_for_capacity) {
                send_cv.wait(lock, [this] {
                    return (in_flight_sends < kMaxInFlightSends &&
                            in_flight_bytes < kMaxInFlightBytes) ||
                           transport_down.load();
                });
            } else if (in_flight_sends >= kMaxInFlightSends ||
                       in_flight_bytes >= kMaxInFlightBytes) {
                return Error(Status::WaitTimeout, "send queue is full");
            }
            if (transport_down.load()) {
                return Error(Status::Unavailable, "connection closed before send");
            }
            ++in_flight_sends;
            in_flight_bytes += bytes.size();
        }

        auto* pending = new PendingSend;
        pending->bytes = std::move(bytes);
        pending->buffer.Buffer = pending->bytes.data();
        pending->buffer.Length = static_cast<uint32_t>(pending->bytes.size());
        const QUIC_STATUS status =
            api->StreamSend(stream, &pending->buffer, 1, flags, pending);
        if (QUIC_FAILED(status)) {
            release_send(pending->bytes.size());
            delete pending;
            // A send that aborts is the stream or connection already closing,
            // which for the host means the viewer is gone. Not a failure.
            if (status == QUIC_STATUS_ABORTED || status == QUIC_STATUS_INVALID_STATE) {
                return Error(Status::Unavailable, "the connection closed before the send");
            }
            return quic_error(status, "QUIC StreamSend");
        }
        return Error::success();
    }

    Error send_bytes(HQUIC stream, std::vector<uint8_t> bytes, QUIC_SEND_FLAGS flags) {
        return send_bytes_impl(stream, std::move(bytes), flags, true);
    }

    bool try_send_bytes(HQUIC stream, std::vector<uint8_t> bytes, QUIC_SEND_FLAGS flags) {
        return send_bytes_impl(stream, std::move(bytes), flags, false).ok();
    }

    bool wait_sends(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(send_mutex);
        return send_cv.wait_for(lock, timeout, [this] {
            return in_flight_sends == 0 || transport_down.load();
        });
    }

    std::string remote_address() const {
        HQUIC handle = connection.load();
        if (!handle) {
            return {};
        }
        QUIC_ADDR address{};
        uint32_t size = sizeof(address);
        if (QUIC_FAILED(api->GetParam(handle, QUIC_PARAM_CONN_REMOTE_ADDRESS, &size, &address))) {
            return {};
        }
        return format_address(address);
    }

    // Answers a refused Hello and frees the connection slot so the listener
    // can accept the next client. Application thread only: the denial is sent
    // and the connection closed outside any MsQuic callback.
    void refuse_client(AccessDenial code, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            hello_ready = false;
        }
        if (HQUIC control_handle = control.load()) {
            send_bytes(control_handle, make_access_denied(code, reason), QUIC_SEND_FLAG_NONE);
            wait_sends(std::chrono::milliseconds(500));
        }
        // Hold the connection open until the refused client has read the
        // denial and gone away: closing it here races the message, and the
        // client then only sees the transport drop instead of the reason.
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(750), [this] {
                return peer_closed || shutdown_complete || transport_failed;
            });
        }
        reset_connection();
    }

    // Closes the current connection and clears everything a handshake set, so
    // the session is ready for another client.
    void reset_connection() {
        HQUIC connection_handle = connection.load();
        if (connection_handle) {
            bool complete = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                complete = shutdown_complete;
            }
            if (!complete) {
                api->ConnectionShutdown(connection_handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(1500),
                            [this] { return shutdown_complete; });
            }
        }
        wait_sends(std::chrono::milliseconds(500));
        if (HQUIC handle = audio.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (HQUIC handle = video.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (HQUIC handle = control.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (connection_handle) {
            // No callback runs for this handle after ConnectionClose returns,
            // so the state can be cleared without a late event overwriting it.
            api->ConnectionClose(connection_handle);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = false;
            shutdown_complete = false;
            peer_closed = false;
            video_finished = false;
            control_finished = false;
            audio_finished = false;
            video_shutdown_complete = false;
            control_shutdown_complete = false;
            audio_shutdown_complete = false;
            audio_stopped = false;
            hello_ready = false;
            hello_ack_ready = false;
            pending_denial = false;
            requested_display = kDisplayAuto;
            requested_peer.clear();
            peer_info = ServerInfo{};
            peer_stats = ClientStats{};
            stream_update_ready = false;
            stream_updates = 0;
            keyframe_requested = false;
            announced = false;
            stats_ready = false;
            bye_ready = false;
            input_events.clear();
            input_releases = 0;
            clipboard_updates.clear();
            clipboard_received = 0;
            cursor_update.reset();
            cursor_received = 0;
            latency = LatencySample{};
            last_reply_probe_us = 0;
            received.clear();
            audio_received.clear();
            control_parser.reset();
            frame_parser.reset();
            audio_parser.reset();
            unclassified.clear();
            transport_failed = false;
            transport_status = Status::Ok;
            transport_message.clear();
        }
        transport_down = false;
        next_audio_seq = 0;
        // Only now can the listener claim the slot for the next client.
        connection = nullptr;
        cv.notify_all();
        send_cv.notify_all();
    }

    // Spends a one-time token once a client was accepted.
    void consume_token() {
        if (server_config.access_token.empty() || server_config.reuse_access_token) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        token_consumed = true;
    }

    Error shutdown_connection(std::chrono::milliseconds timeout) {
        HQUIC handle = connection.load();
        if (!handle) {
            return Error::success();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (shutdown_complete) {
                return Error::success();
            }
        }
        api->ConnectionShutdown(handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, timeout, [this] { return shutdown_complete; })) {
            return Error(Status::WaitTimeout, "QUIC connection did not shut down");
        }
        return Error::success();
    }

    void close() {
        if (!api) {
            return;
        }
        // Stops the injection thread and lifts anything it was holding, so a
        // disconnect never leaves a key or button down on this machine.
        if (injector) {
            injector->stop();
        }
        HQUIC connection_handle = connection.load();
        if (connection_handle) {
            bool complete = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                complete = shutdown_complete;
            }
            if (!complete) {
                api->ConnectionShutdown(connection_handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(2000),
                            [this] { return shutdown_complete; });
            }
        }
        wait_sends(std::chrono::milliseconds(1000));

        // A server-initiated stream parked for classification still holds an
        // application reference; left open it would keep the connection alive
        // and RegistrationClose would wait forever.
        std::vector<HQUIC> parked;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& entry : unclassified) {
                parked.push_back(entry.first);
            }
            unclassified.clear();
        }
        for (HQUIC handle : parked) {
            api->StreamClose(handle);
        }

        // StreamClose is the only release for a stream handle, whether or not
        // its SHUTDOWN_COMPLETE was observed; closing before that implicitly
        // aborts, which is what teardown wants.
        if (HQUIC handle = audio.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (HQUIC handle = video.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (HQUIC handle = control.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (listener) {
            api->ListenerClose(listener);
            listener = nullptr;
        }
        if (connection_handle) {
            api->ConnectionClose(connection_handle);
            connection = nullptr;
        }
        if (configuration) {
            api->ConfigurationClose(configuration);
            configuration = nullptr;
        }
        if (registration) {
            api->RegistrationClose(registration);
            registration = nullptr;
        }
        if (certificate) {
            CertFreeCertificateContext(certificate);
            certificate = nullptr;
        }
        MsQuicClose(api);
        api = nullptr;
    }

    Error finish_with_error() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (transport_failed) {
            return Error(transport_status, transport_message);
        }
        return Error::success();
    }
};

QuicSession::~QuicSession() = default;

std::unique_ptr<QuicSession> QuicSession::create_server(const QuicServerConfig& config,
                                                        std::string* error) {
    auto session = std::unique_ptr<QuicSession>(new QuicSession());
    session->impl_ = std::make_unique<Impl>();
    Impl& impl = *session->impl_;
    impl.role = Impl::Role::Server;
    impl.server_config = config;

    QUIC_STATUS status = MsQuicOpen2(&impl.api);
    if (QUIC_FAILED(status)) {
        *error = "MsQuicOpen2 failed: " + status_hex(status);
        return nullptr;
    }
    QUIC_REGISTRATION_CONFIG registration_config{"chast", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    status = impl.api->RegistrationOpen(&registration_config, &impl.registration);
    if (QUIC_FAILED(status)) {
        *error = "QUIC RegistrationOpen failed: " + status_hex(status);
        return nullptr;
    }

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = config.idle_timeout_ms;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.KeepAliveIntervalMs = kKeepAliveMs;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    // The client may open one bidirectional stream: the control channel.
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    settings.IsSet.ServerResumptionLevel = TRUE;

    QUIC_BUFFER alpn{};
    alpn.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(kAlpn));
    alpn.Length = static_cast<uint32_t>(std::strlen(kAlpn));
    status = impl.api->ConfigurationOpen(impl.registration, &alpn, 1, &settings,
                                         sizeof(settings), nullptr, &impl.configuration);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ConfigurationOpen failed: " + status_hex(status);
        return nullptr;
    }

    impl.certificate = open_or_create_identity(config.identity_scope, error);
    if (!impl.certificate) {
        return nullptr;
    }
    impl.identity_fingerprint = certificate_fingerprint(impl.certificate);
    if (impl.identity_fingerprint.empty()) {
        *error = "cannot fingerprint the server identity certificate";
        return nullptr;
    }
    QUIC_CREDENTIAL_CONFIG credential{};
    credential.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
    credential.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    credential.CertificateContext =
        reinterpret_cast<QUIC_CERTIFICATE*>(const_cast<CERT_CONTEXT*>(impl.certificate));
    status = impl.api->ConfigurationLoadCredential(impl.configuration, &credential);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ConfigurationLoadCredential failed: " + status_hex(status);
        return nullptr;
    }

    status = impl.api->ListenerOpen(impl.registration, &Impl::listener_callback,
                                    &impl, &impl.listener);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ListenerOpen failed: " + status_hex(status);
        return nullptr;
    }

    QUIC_ADDR address{};
    if (!QuicAddrFromString(config.bind_address.c_str(), config.port, &address)) {
        *error = "cannot parse the listen address " + config.bind_address;
        return nullptr;
    }
    status = impl.api->ListenerStart(impl.listener, &alpn, 1, &address);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ListenerStart failed on " + config.bind_address + ":" +
                 std::to_string(config.port) + ": " + status_hex(status);
        return nullptr;
    }
    if (config.allow_input) {
        impl.injector = InputInjector::create(config.dry_run_input, error);
        if (!impl.injector) {
            return nullptr;
        }
    }
    return session;
}

std::unique_ptr<QuicSession> QuicSession::create_client(const QuicClientConfig& config,
                                                        std::string* error) {
    auto session = std::unique_ptr<QuicSession>(new QuicSession());
    session->impl_ = std::make_unique<Impl>();
    Impl& impl = *session->impl_;
    impl.role = Impl::Role::Client;
    impl.client_config = config;

    QUIC_STATUS status = MsQuicOpen2(&impl.api);
    if (QUIC_FAILED(status)) {
        *error = "MsQuicOpen2 failed: " + status_hex(status);
        return nullptr;
    }
    QUIC_REGISTRATION_CONFIG registration_config{"chast", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    status = impl.api->RegistrationOpen(&registration_config, &impl.registration);
    if (QUIC_FAILED(status)) {
        *error = "QUIC RegistrationOpen failed: " + status_hex(status);
        return nullptr;
    }

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = config.idle_timeout_ms;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.KeepAliveIntervalMs = kKeepAliveMs;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    // The server opens one unidirectional video stream and, when it shares
    // system audio, a second for audio. Either may send first.
    settings.PeerUnidiStreamCount = 2;
    settings.IsSet.PeerUnidiStreamCount = TRUE;

    QUIC_BUFFER alpn{};
    alpn.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(kAlpn));
    alpn.Length = static_cast<uint32_t>(std::strlen(kAlpn));
    status = impl.api->ConfigurationOpen(impl.registration, &alpn, 1, &settings,
                                         sizeof(settings), nullptr, &impl.configuration);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ConfigurationOpen failed: " + status_hex(status);
        return nullptr;
    }

    const std::string pin = normalize_fingerprint(config.pinned_fingerprint);
    if (!config.pinned_fingerprint.empty() && pin.empty()) {
        *error = "the pinned server fingerprint is not a SHA-256 value";
        return nullptr;
    }
    if (pin.empty() && !config.allow_any_certificate) {
        *error = "no pinned server identity: set pinned_fingerprint, or "
                 "allow_any_certificate for local development";
        return nullptr;
    }
    impl.pinned_fingerprint = pin;
    impl.allow_any_certificate = config.allow_any_certificate;

    QUIC_CREDENTIAL_CONFIG credential{};
    credential.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credential.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                       QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
                       QUIC_CREDENTIAL_FLAG_DEFER_CERTIFICATE_VALIDATION;
    if (impl.api->ConnectionCertificateValidationComplete == nullptr) {
        *error = "MsQuic does not support the certificate validation callback";
        return nullptr;
    }
    status = impl.api->ConfigurationLoadCredential(impl.configuration, &credential);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ConfigurationLoadCredential failed: " + status_hex(status);
        return nullptr;
    }

    HQUIC connection = nullptr;
    status = impl.api->ConnectionOpen(impl.registration, &Impl::connection_callback, &impl,
                                      &connection);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ConnectionOpen failed: " + status_hex(status);
        return nullptr;
    }
    impl.connection = connection;
    return session;
}

Error QuicSession::wait_for_client(std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        std::unique_lock<std::mutex> lock(impl.mutex);
        const bool woken = impl.cv.wait_until(lock, deadline, [&impl] {
            return impl.hello_ready || impl.pending_denial || impl.transport_failed ||
                   impl.shutdown_complete || impl.peer_closed;
        });
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.pending_denial) {
            const AccessDenial code = impl.pending_code;
            const std::string reason = impl.pending_reason;
            impl.pending_denial = false;
            lock.unlock();
            impl.refuse_client(code, reason);
            if (std::chrono::steady_clock::now() >= deadline) {
                return Error(Status::WaitTimeout, "no client connected in time");
            }
            continue;
        }        if (impl.hello_ready) {
            // Consumed here: the next wait must be triggered by a new Hello,
            // not by the one this call already answered.
            impl.hello_ready = false;
            if (!impl.server_config.approve_client) {
                lock.unlock();
                impl.consume_token();
                return Error::success();
            }
            const ClientRequest request{impl.requested_display, impl.requested_peer};
            lock.unlock();
            if (impl.server_config.approve_client(request)) {
                impl.consume_token();
                return Error::success();
            }
            impl.refuse_client(AccessDenial::Declined, "the host declined the connection");
            if (std::chrono::steady_clock::now() >= deadline) {
                return Error(Status::WaitTimeout, "no client connected in time");
            }
            continue;
        }
        if (woken) {
            // A connection that came and went without a usable Hello (aborted
            // TLS, no Hello, wrong protocol version) frees the slot; the
            // listener stays available for the next client.
            lock.unlock();
            impl.reset_connection();
            if (std::chrono::steady_clock::now() >= deadline) {
                return Error(Status::WaitTimeout, "no client connected in time");
            }
            continue;
        }
        return Error(Status::WaitTimeout, "no client connected in time");
    }
}

std::optional<AccessDenial> QuicSession::denial() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->denial_code;
}

uint16_t QuicSession::requested_display() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->requested_display;
}

std::string QuicSession::server_fingerprint() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->identity_fingerprint;
}

std::string QuicSession::client_address() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->requested_peer;
}

void QuicSession::set_access_token(std::string token) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->server_config.access_token = std::move(token);
    impl_->token_consumed = false;
}

void QuicSession::reset_for_next_client() {
    impl_->reset_connection();
}

std::string QuicSession::peer_fingerprint() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->peer_fingerprint;
}

Error QuicSession::announce(const ServerInfo& info) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "announce is a server operation");
    }
    if (impl.announced) {
        return Error(Status::InvalidCall, "the stream was already announced");
    }
    ServerInfo ack = info;
    // The gates belong to the server; never let a caller announce input the
    // session will not inject or audio the host disabled.
    ack.input_enabled = impl.server_config.allow_input;
    ack.audio.enabled = ack.audio.enabled && impl.server_config.allow_audio;
    if (!ack.audio.enabled) {
        ack.audio.extradata.clear();
    }

    // Open both media streams before the HelloAck so the announcement never
    // describes a stream that failed to open. The client classifies them by
    // packet magic, so their order here does not matter.
    HQUIC video = nullptr;
    QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &Impl::stream_callback, &impl, &video);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC StreamOpen(video)");
    }
    status = impl.api->StreamStart(video, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        impl.api->StreamClose(video);
        return quic_error(status, "QUIC StreamStart(video)");
    }
    impl.video = video;

    if (ack.audio.enabled) {
        HQUIC audio = nullptr;
        QUIC_STATUS audio_status = impl.api->StreamOpen(
            impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            &Impl::stream_callback, &impl, &audio);
        if (QUIC_SUCCEEDED(audio_status)) {
            audio_status =
                impl.api->StreamStart(audio, QUIC_STREAM_START_FLAG_NONE);
        }
        if (QUIC_FAILED(audio_status)) {
            if (audio) {
                impl.api->StreamClose(audio);
            }
            // Audio is optional: announce video only instead of failing a
            // session that could still stream the desktop.
            ack.audio.enabled = false;
            ack.audio.extradata.clear();
        } else {
            impl.audio = audio;
            impl.next_audio_seq = 0;
        }
    }

    impl.announced = true;
    Error sent = impl.send_bytes(impl.control.load(), make_hello_ack(ack),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        if (HQUIC handle = impl.audio.exchange(nullptr)) {
            impl.api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            impl.api->StreamClose(handle);
        }
        if (HQUIC handle = impl.video.exchange(nullptr)) {
            impl.api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            impl.api->StreamClose(handle);
        }
        impl.announced = false;
        return sent;
    }
    return Error::success();
}

Error QuicSession::refresh_stream(const ServerInfo& info) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "refresh_stream is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream was not announced yet");
    }
    ServerInfo update = info;
    update.input_enabled = impl.server_config.allow_input;
    bool audio_stream_open = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        audio_stream_open = impl.audio.load() != nullptr && !impl.audio_stopped;
    }
    update.audio.enabled =
        update.audio.enabled && impl.server_config.allow_audio && audio_stream_open;
    if (!update.audio.enabled) {
        update.audio.extradata.clear();
    }
    return impl.send_bytes(impl.control.load(), make_stream_info(update),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_stream_update(ServerInfo& info) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->stream_update_ready) {
        return false;
    }
    info = impl_->peer_info;
    impl_->stream_update_ready = false;
    return true;
}

uint64_t QuicSession::stream_updates() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stream_updates;
}

Error QuicSession::send_frame(FrameHeader header, const uint8_t* payload, size_t size) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_frame is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the video stream is not open");
    }
    if (!payload || size == 0 || size > kMaxFramePayload) {
        return Error(Status::InvalidCall, "invalid frame payload size");
    }
    HQUIC video = impl.video.load();
    if (!video) {
        return Error(Status::Unavailable, "the video stream is gone");
    }
    header.seq = impl.next_seq.fetch_add(1);
    header.payload_size = static_cast<uint32_t>(size);
    header.payload_crc32 = crc32(payload, size);
    std::vector<uint8_t> bytes(kFrameHeaderSize + size);
    encode_frame_header(header, bytes.data());
    std::memcpy(bytes.data() + kFrameHeaderSize, payload, size);
    return impl.send_bytes(video, std::move(bytes), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_keyframe_request() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool requested = impl_->keyframe_requested;
    impl_->keyframe_requested = false;
    return requested;
}

Error QuicSession::send_audio(AudioHeader header, const uint8_t* payload, size_t size) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_audio is a server operation");
    }
    // A closed gate is a caller mistake, not a lost stream; keep the two
    // cases distinguishable for the host and the loopback check.
    if (!impl.server_config.allow_audio) {
        return Error(Status::InvalidCall, "audio is disabled on this server");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the audio stream is not open");
    }
    if (!payload || size == 0 || size > kMaxAudioPayload) {
        return Error(Status::InvalidCall, "invalid audio payload size");
    }
    HQUIC audio = impl.audio.load();
    if (!audio) {
        return Error(Status::Unavailable, "the audio stream is gone");
    }
    header.seq = impl.next_audio_seq.fetch_add(1);
    header.payload_size = static_cast<uint32_t>(size);
    header.payload_crc32 = crc32(payload, size);
    std::vector<uint8_t> bytes(kAudioHeaderSize + size);
    encode_audio_header(header, bytes.data());
    std::memcpy(bytes.data() + kAudioHeaderSize, payload, size);
    // Never waits: a full queue would otherwise stall the capture loop and,
    // with it, video. Dropping a packet is a short gap in the AAC stream.
    return impl.send_bytes_impl(audio, std::move(bytes), QUIC_SEND_FLAG_NONE, false);
}

Error QuicSession::stop_audio() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "stop_audio is a server operation");
    }
    HQUIC audio = impl.audio.load();
    if (!audio) {
        return Error(Status::InvalidCall, "the audio stream is not open");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.audio_stopped) {
            return Error::success();
        }
        impl.audio_stopped = true;
    }
    impl.api->StreamShutdown(audio, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    return Error::success();
}

std::vector<InputEvent> QuicSession::take_input_events() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<InputEvent> events(impl_->input_events.begin(), impl_->input_events.end());
    impl_->input_events.clear();
    return events;
}

InputStats QuicSession::input_stats() const {
    return impl_->injector ? impl_->injector->stats() : InputStats{};
}

uint64_t QuicSession::input_release_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->input_releases;
}

Error QuicSession::finish_server(std::chrono::milliseconds timeout, ClientStats& stats,
                                 bool& stats_received) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "finish_server is a server operation");
    }
    if (HQUIC video = impl.video.load()) {
        impl.api->StreamShutdown(video, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    }
    if (HQUIC audio = impl.audio.load()) {
        impl.api->StreamShutdown(audio, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    }
    impl.wait_sends(timeout / 2);

    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        impl.cv.wait_for(lock, timeout, [&impl] {
            return impl.stats_ready || impl.bye_ready || impl.transport_failed ||
                   impl.shutdown_complete;
        });
        stats_received = impl.stats_ready;
        if (stats_received) {
            stats = impl.peer_stats;
        }
    }

    Error shutdown = impl.shutdown_connection(timeout);
    if (!shutdown.ok()) {
        return shutdown;
    }
    return impl.finish_with_error();
}

Error QuicSession::connect(std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "connect is a client operation");
    }
    const QUIC_STATUS status = impl.api->ConnectionStart(
        impl.connection.load(), impl.configuration, QUIC_ADDRESS_FAMILY_UNSPEC,
        impl.client_config.host.c_str(), impl.client_config.port);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC ConnectionStart");
    }
    std::unique_lock<std::mutex> lock(impl.mutex);
    if (!impl.cv.wait_for(lock, timeout, [&impl] {
            return impl.connected || impl.transport_failed || impl.shutdown_complete;
        })) {
        return Error(Status::WaitTimeout, "QUIC handshake timed out");
    }
    if (impl.transport_failed) {
        return Error(impl.transport_status, impl.transport_message);
    }
    if (!impl.connected) {
        return Error(Status::Unavailable, "the server closed the connection");
    }
    return Error::success();
}

Error QuicSession::handshake(ServerInfo& info, std::chrono::milliseconds timeout,
                             uint16_t display) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "handshake is a client operation");
    }
    HQUIC control = nullptr;
    QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_NONE, &Impl::stream_callback, &impl,
        &control);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC StreamOpen(control)");
    }
    impl.control = control;
    status = impl.api->StreamStart(control, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        impl.control = nullptr;
        impl.api->StreamClose(control);
        return quic_error(status, "QUIC StreamStart(control)");
    }

    Error sent = impl.send_bytes(
        control, make_hello(display, impl.client_config.access_token), QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        return sent;
    }
    std::unique_lock<std::mutex> lock(impl.mutex);
    impl.cv.wait_for(lock, timeout, [&impl] {
        return impl.hello_ack_ready || impl.access_denied_ready || impl.transport_failed ||
               impl.shutdown_complete || impl.peer_closed;
    });
    // A refusal arrives just before the server closes the connection, so it
    // is checked first: the transport error that follows is only a symptom.
    if (impl.access_denied_ready) {
        return Error(Status::PermissionDenied, impl.denial_reason);
    }
    if (impl.transport_failed) {
        return Error(impl.transport_status, impl.transport_message);
    }
    if (!impl.hello_ack_ready) {
        return Error(Status::Unavailable, "the server did not answer Hello");
    }
    info = impl.peer_info;
    return Error::success();
}

Error QuicSession::receive_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                                 std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "receive_frame is a client operation");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    for (;;) {
        while (!impl.received.empty()) {
            std::vector<uint8_t> chunk = std::move(impl.received.front());
            impl.received.pop_front();
            lock.unlock();
            std::string error;
            const bool ok = impl.frame_parser.push(chunk.data(), chunk.size(), &error);
            lock.lock();
            if (!ok) {
                return Error(Status::CaptureFailed, "video stream: " + error);
            }
        }

        const uint8_t* frame_payload = nullptr;
        std::string parse_error;
        switch (impl.frame_parser.next(header, &frame_payload, &parse_error)) {
            case FrameParser::Status::Frame: {
                payload.assign(frame_payload, frame_payload + header.payload_size);
                impl.frame_parser.consume();
                return Error::success();
            }
            case FrameParser::Status::Error:
                return Error(Status::CaptureFailed, "video stream: " + parse_error);
            case FrameParser::Status::Incomplete:
                break;
        }

        if (impl.video_finished) {
            if (impl.frame_parser.buffered() > 0) {
                return Error(Status::CaptureFailed, "video stream ended mid-frame");
            }
            return Error(Status::Unavailable, "the server finished the video stream");
        }
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.shutdown_complete) {
            return Error(Status::Unavailable, "the connection closed");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(Status::WaitTimeout, "no frame arrived in time");
        }
        impl.cv.wait_until(lock, deadline);
    }
}

Error QuicSession::receive_audio(AudioHeader& header, std::vector<uint8_t>& payload,
                                 std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "receive_audio is a client operation");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    for (;;) {
        while (!impl.audio_received.empty()) {
            std::vector<uint8_t> chunk = std::move(impl.audio_received.front());
            impl.audio_received.pop_front();
            lock.unlock();
            std::string error;
            const bool ok = impl.audio_parser.push(chunk.data(), chunk.size(), &error);
            lock.lock();
            if (!ok) {
                return Error(Status::CaptureFailed, "audio stream: " + error);
            }
        }

        const uint8_t* audio_payload = nullptr;
        std::string parse_error;
        switch (impl.audio_parser.next(header, &audio_payload, &parse_error)) {
            case AudioParser::Status::Packet: {
                payload.assign(audio_payload, audio_payload + header.payload_size);
                impl.audio_parser.consume();
                return Error::success();
            }
            case AudioParser::Status::Error:
                return Error(Status::CaptureFailed, "audio stream: " + parse_error);
            case AudioParser::Status::Incomplete:
                break;
        }

        if (impl.audio_finished) {
            if (impl.audio_parser.buffered() > 0) {
                return Error(Status::CaptureFailed, "audio stream ended mid-packet");
            }
            return Error(Status::Unavailable, "the server finished the audio stream");
        }
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.shutdown_complete) {
            return Error(Status::Unavailable, "the connection closed");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(Status::WaitTimeout, "no audio arrived in time");
        }
        impl.cv.wait_until(lock, deadline);
    }
}

Error QuicSession::request_keyframe() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_keyframe is a client operation");
    }
    return impl.send_bytes(impl.control.load(), make_control(ControlType::KeyframeRequest, {}),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::send_latency_probe() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "send_latency_probe is a client operation");
    }
    Error sent = impl.send_bytes(impl.control.load(), make_latency_probe(steady_us()),
                                 QUIC_SEND_FLAG_NONE);
    if (sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        ++impl.latency.probes_sent;
    }
    return sent;
}

LatencySample QuicSession::latency() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->latency;
}

TransportStats QuicSession::transport_stats() const {
    Impl& impl = *impl_;
    TransportStats stats;
    {
        std::lock_guard<std::mutex> lock(impl.send_mutex);
        stats.queued_bytes = impl.in_flight_bytes;
        stats.queued_sends = impl.in_flight_sends;
    }
    HQUIC handle = impl.connection.load();
    if (!handle) {
        return stats;
    }
    QUIC_STATISTICS_V2 v2{};
    uint32_t size = sizeof(v2);
    if (QUIC_SUCCEEDED(
            impl.api->GetParam(handle, QUIC_PARAM_CONN_STATISTICS_V2, &size, &v2))) {
        stats.available = true;
        stats.rtt_us = v2.Rtt;
        stats.min_rtt_us = v2.MinRtt;
        stats.max_rtt_us = v2.MaxRtt;
        stats.send_bytes = v2.SendTotalBytes;
        stats.recv_bytes = v2.RecvTotalBytes;
        stats.sent_packets = v2.SendTotalPackets;
        stats.lost_packets = v2.SendSuspectedLostPackets >= v2.SendSpuriousLostPackets
                                 ? v2.SendSuspectedLostPackets - v2.SendSpuriousLostPackets
                                 : 0;
        stats.congestion_window = v2.SendCongestionWindow;
        return stats;
    }
    // Older MsQuic builds expose only the v1 layout; RTT and bytes are still
    // useful, the congestion window is not part of it.
    QUIC_STATISTICS v1{};
    size = sizeof(v1);
    if (QUIC_SUCCEEDED(impl.api->GetParam(handle, QUIC_PARAM_CONN_STATISTICS, &size, &v1))) {
        stats.available = true;
        stats.rtt_us = v1.Rtt;
        stats.min_rtt_us = v1.MinRtt;
        stats.max_rtt_us = v1.MaxRtt;
        stats.send_bytes = v1.Send.TotalBytes;
        stats.recv_bytes = v1.Recv.TotalBytes;
        stats.sent_packets = v1.Send.TotalPackets;
        stats.lost_packets = v1.Send.SuspectedLostPackets >= v1.Send.SpuriousLostPackets
                                 ? v1.Send.SuspectedLostPackets - v1.Send.SpuriousLostPackets
                                 : 0;
    }
    return stats;
}

Error QuicSession::send_input(const InputEvent& event) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "send_input is a client operation");
    }
    return impl.send_bytes(impl.control.load(), make_input_event(event), QUIC_SEND_FLAG_NONE);
}

Error QuicSession::release_input() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "release_input is a client operation");
    }
    return impl.send_bytes(impl.control.load(), make_control(ControlType::InputRelease, {}),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::send_clipboard(uint64_t seq, const std::string& text) {
    Impl& impl = *impl_;
    if (impl.role == Impl::Role::Server && !impl.server_config.allow_clipboard) {
        return Error(Status::InvalidCall, "clipboard is disabled on this server");
    }
    std::vector<uint8_t> message = make_clipboard_update(seq, text);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid clipboard text");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_clipboard_update(ClipboardUpdate& update) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->clipboard_updates.empty()) {
        return false;
    }
    update = std::move(impl_->clipboard_updates.front());
    impl_->clipboard_updates.pop_front();
    return true;
}

uint64_t QuicSession::clipboard_updates_received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->clipboard_received;
}

Error QuicSession::send_cursor(const CursorUpdate& update) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_cursor is a server operation");
    }
    std::vector<uint8_t> message = make_cursor_update(update);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid cursor update");
    }
    // Never waits: a pointer that is already stale is worth dropping, and the
    // capture loop must not stall behind it. The next frame sends the current
    // position again.
    return impl.send_bytes_impl(impl.control.load(), std::move(message),
                                QUIC_SEND_FLAG_NONE, false);
}

bool QuicSession::take_cursor_update(CursorUpdate& update) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->cursor_update) {
        return false;
    }
    update = std::move(*impl_->cursor_update);
    impl_->cursor_update.reset();
    return true;
}

uint64_t QuicSession::cursor_updates_received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->cursor_received;
}

Error QuicSession::finish_client(const ClientStats& stats, std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "finish_client is a client operation");
    }
    HQUIC control = impl.control.load();
    Error sent = impl.send_bytes(control, make_stats(stats), QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        return sent;
    }
    sent = impl.send_bytes(control, make_control(ControlType::Bye, {}), QUIC_SEND_FLAG_FIN);
    if (!sent.ok()) {
        return sent;
    }
    impl.wait_sends(timeout / 2);

    // The server closes the connection after it has seen Bye; waiting for that
    // keeps the client from discarding its own last messages.
    bool peer_shutdown = false;
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        impl.cv.wait_for(lock, timeout, [&impl] {
            return impl.shutdown_complete || impl.transport_failed;
        });
        peer_shutdown = impl.shutdown_complete;
    }
    if (peer_shutdown) {
        return impl.finish_with_error();
    }
    Error shutdown = impl.shutdown_connection(std::chrono::milliseconds(2000));
    if (!shutdown.ok()) {
        return shutdown;
    }
    return impl.finish_with_error();
}

bool QuicSession::video_finished() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->video_finished;
}

bool QuicSession::audio_finished() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->audio_finished;
}

Error QuicSession::async_error() const {
    return impl_->recorded_error();
}

}  // namespace chast
