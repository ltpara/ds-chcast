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
#include <ctime>
#include <deque>
#include <map>
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

// Receive-side bounds for the protocol v16 streams. They are deliberately
// larger than one legitimate item so a slow consumer is not punished, but
// small enough that a peer cannot turn the session into an allocator.
constexpr size_t kMaxClipboardStreams = 8;
constexpr size_t kMaxPendingClipboardBytes = 96u * 1024u * 1024u;
constexpr size_t kMaxClipboardStreamBytes = kMaxClipboardTotalBytes + 64u * 1024u;
constexpr size_t kMaxIncomingUploads = 8;
constexpr size_t kMaxBufferedFileBytes = 64u * 1024u * 1024u;
// The host stops consuming CFIL bytes above the high-water mark (leaving the
// rest with MsQuic, which then withholds flow-control credit) and resumes
// below the low-water mark. A slow poll becomes backpressure on the viewer
// instead of an overflow that drops the transfer.
constexpr size_t kFileReceiveHighWaterBytes = 32u * 1024u * 1024u;
constexpr size_t kFileReceiveLowWaterBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxQueuedOffers = 8;

// Device names come from the local computer and the protocol caps them at
// kMaxDeviceNameLength bytes. Cut on a whole UTF-8 sequence so a long name
// never produces a Hello the host rejects as malformed.
std::string truncate_utf8(const std::string& text, size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    size_t cut = limit;
    while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut);
}

uint16_t permission_mask_from_info(const ServerInfo& info) {
    uint16_t mask = 0;
    if (info.input_enabled) {
        mask |= kPermissionInput;
    }
    if (info.clipboard_enabled) {
        mask |= kPermissionClipboard;
    }
    if (info.audio.enabled) {
        mask |= kPermissionAudio;
    }
    if (info.file_transfer_enabled) {
        mask |= kPermissionFileTransfer;
    }
    if (info.microphone_enabled) {
        mask |= kPermissionMicrophone;
    }
    return mask;
}

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
    // Server: the accepted viewer-initiated CMIC stream. Client: the stream
    // this side opened for the microphone. Null while the microphone is off.
    std::atomic<HQUIC> microphone{nullptr};

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
    bool microphone_finished = false;
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
    // Server side: what Hello asked to have captured, and the full Hello for
    // the application thread to authenticate. The decision is made there
    // because hashing a password or prompting an operator must not run on a
    // MsQuic worker, and closing the connection from its own callback is not
    // allowed.
    uint16_t requested_display = kDisplayAuto;
    std::string requested_peer;
    ClientHello pending_hello;
    // Server: the newest SwitchDisplay the viewer asked for. Intermediate
    // targets are not queued; a viewer clicking through the list wants the
    // last one, not a tour of every output in between.
    bool display_switch_ready = false;
    uint16_t display_switch_index = kDisplayAuto;
    // Client: the newest DisplaySwitchResult. Like the request, only the
    // newest answer matters; an older one describes a target already left.
    bool display_switch_result_ready = false;
    uint16_t display_switch_result_index = 0;
    DisplaySwitchCode display_switch_code = DisplaySwitchCode::Ok;
    std::string display_switch_reason;
    uint64_t display_switch_results_seen = 0;
    // Server: the newest mode list request and mode change request. Client:
    // the newest answers, for the same reason as the switch pair above.
    bool display_modes_request_ready = false;
    uint16_t display_modes_request_display = 0;
    bool display_modes_ready = false;
    DisplayModeList display_modes;
    uint64_t display_mode_lists_seen = 0;
    bool set_display_mode_ready = false;
    uint16_t set_display_mode_index = 0;
    uint16_t set_display_mode_width = 0;
    uint16_t set_display_mode_height = 0;
    uint16_t set_display_mode_refresh = 0;
    bool display_mode_result_ready = false;
    uint16_t display_mode_result_index = 0;
    uint16_t display_mode_result_width = 0;
    uint16_t display_mode_result_height = 0;
    uint16_t display_mode_result_refresh = 0;
    DisplayModeCode display_mode_code = DisplayModeCode::Ok;
    std::string display_mode_reason;
    uint64_t display_mode_results_seen = 0;
    // Server: the mask the viewer was granted, and the ceiling that grant
    // established. Mid-session changes may only stay inside the ceiling.
    // Client: the mask the host last announced. Atomic because the transport
    // callbacks read it while the application thread updates it.
    std::atomic<uint16_t> session_permissions{0};
    uint16_t permission_ceiling = 0;
    // Client: a PermissionUpdate arrived since take_permission_update().
    bool permission_update_ready = false;
    uint64_t permission_updates = 0;
    // Client: HelloAck was accepted, so the permission mask is meaningful.
    bool hello_complete = false;
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
    // Microphone uplink (CMIC). `microphone_handles` keeps every CMIC handle
    // this side accepted, including one that was replaced by a newer stream,
    // so late events close the old handle instead of being classified again.
    std::deque<std::vector<uint8_t>> microphone_received;
    AudioParser microphone_parser;
    AudioInfo microphone_info;
    bool microphone_format_changed = false;
    std::vector<HQUIC> microphone_handles;
    uint64_t microphone_streams_seen = 0;
    // Client: server-initiated streams whose first bytes have not arrived yet.
    // The packet magic, not the stream order, says which one is which.
    std::vector<std::pair<HQUIC, std::vector<uint8_t>>> unclassified;
    // Server only, when QuicServerConfig::allow_input is set.
    std::unique_ptr<InputInjector> injector;
    std::deque<InputEvent> input_events;
    size_t input_releases = 0;
    std::deque<ClipboardUpdate> clipboard_updates;
    uint64_t clipboard_received = 0;
    // Protocol v16 clipboard and file streams.
    struct ClipboardStream {
        HQUIC stream = nullptr;
        std::vector<uint8_t> bytes;
        bool finished = false;
    };
    struct ReceivedUpload {
        HQUIC stream = nullptr;
        uint64_t start_offset = 0;
        uint64_t received = 0;
        bool finished = false;
        bool receive_paused = false;
        std::string error;
    };
    struct OutgoingUpload {
        FileOffer offer;
        bool accepted = false;
        bool started = false;
        bool finished = false;
        HQUIC stream = nullptr;
        uint64_t next_offset = 0;
    };
    std::vector<ClipboardStream> clipboard_streams;
    size_t pending_clipboard_bytes = 0;
    std::deque<ClipboardContent> clipboard_contents;
    uint64_t clipboard_contents_seen = 0;
    std::deque<FileOffer> file_offers;
    uint64_t file_offers_seen = 0;
    std::deque<FileChunk> file_chunks;
    size_t buffered_file_bytes = 0;
    std::map<uint32_t, ReceivedUpload> received_uploads;
    std::deque<FileCancel> file_cancels;
    std::map<uint32_t, OutgoingUpload> outgoing_uploads;
    std::deque<FileAnswer> file_answers;
    std::deque<FileResult> file_results;
    // Unidirectional streams this side opened for clipboard changes; their
    // handles are closed when QUIC reports SHUTDOWN_COMPLETE.
    std::vector<HQUIC> one_shot_streams;
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
    std::atomic<uint64_t> next_microphone_seq{0};
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

    // Whether the connection that carried the pending Hello is still usable.
    // The approval callback may poll this from the application thread while
    // the MsQuic workers update the flags under `mutex`.
    bool peer_alive() const {
        if (transport_down.load()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        return !peer_closed && !shutdown_complete && !transport_failed;
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
                } else if (unidirectional) {
                    if (role == Role::Server &&
                        (session_permissions.load() &
                         (kPermissionClipboard | kPermissionFileTransfer |
                          kPermissionMicrophone)) == 0) {
                        // Neither clipboard, file nor microphone streams are
                        // granted for this session; the HelloAck said so
                        // already.
                        api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                        break;
                    }
                    // Which stream this is cannot be told from the start
                    // event: QUIC does not order unidirectional streams' first
                    // bytes. Park it until a RECEIVE names its magic.
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

    // Buffers a stream until its packet magic identifies it: video/audio on
    // the viewer, clipboard changes and file transfers on both ends, the
    // viewer's microphone on the host. Anything else is a peer that is not
    // speaking this protocol version.
    void classify_media_stream(HQUIC stream, QUIC_STREAM_EVENT& event) {
        enum class Kind { Wait, Video, Audio, Microphone, Clipboard, File, Reject, Bad };
        Kind kind = Kind::Wait;
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
            if (std::memcmp(pending->data(), "CFRM", 4) == 0) {
                HQUIC expected = nullptr;
                if (role == Role::Client && video.compare_exchange_strong(expected, stream)) {
                    received.push_back(std::move(*pending));
                    kind = Kind::Video;
                } else {
                    kind = Kind::Bad;
                }
            } else if (std::memcmp(pending->data(), "CAUD", 4) == 0) {
                HQUIC expected = nullptr;
                if (role == Role::Client && audio.compare_exchange_strong(expected, stream)) {
                    audio_received.push_back(std::move(*pending));
                    kind = Kind::Audio;
                } else {
                    kind = Kind::Bad;
                }
            } else if (std::memcmp(pending->data(), kMicrophoneStreamMagic, 4) == 0) {
                // Viewer to host only, and only while the grant is live. A
                // stream without the permission is refused quietly: the
                // microphone is optional and never takes video down.
                if (role != Role::Server ||
                    (session_permissions.load() & kPermissionMicrophone) == 0) {
                    // Taking the handle into the microphone list means its
                    // abort and completion are closed here like any other
                    // CMIC stream instead of being reported as fatal.
                    microphone_handles.push_back(stream);
                    kind = Kind::Reject;
                } else if (microphone.load() != nullptr && !microphone_finished) {
                    // One microphone stream per viewer session. A second one
                    // is a viewer bug; abort it and keep the first.
                    microphone_handles.push_back(stream);
                    kind = Kind::Reject;
                } else if (pending->size() < kMicrophoneStreamHeaderSize) {
                    return;
                } else {
                    const size_t header_length =
                        microphone_stream_header_length(pending->data(), pending->size());
                    if (header_length == 0) {
                        kind = Kind::Bad;
                    } else if (pending->size() < header_length) {
                        // The fixed part named its extradata but the rest of
                        // the header has not arrived yet.
                        return;
                    } else {
                        AudioInfo info;
                        size_t consumed = 0;
                        std::string error;
                        if (!decode_microphone_stream_header(pending->data(), pending->size(),
                                                             info, consumed, &error)) {
                            kind = Kind::Bad;
                        } else {
                            // A finished stream's handle may still be waiting
                            // for SHUTDOWN_COMPLETE; replacing the active
                            // pointer starts a fresh decode timeline and the
                            // old handle is closed by its own completion.
                            microphone = stream;
                            microphone_handles.push_back(stream);
                            microphone_finished = false;
                            microphone_info = std::move(info);
                            microphone_format_changed = true;
                            microphone_parser.reset();
                            microphone_received.clear();
                            if (pending->size() > consumed) {
                                microphone_received.emplace_back(pending->begin() + consumed,
                                                                 pending->end());
                            }
                            ++microphone_streams_seen;
                            kind = Kind::Microphone;
                        }
                    }
                }
            } else if (std::memcmp(pending->data(), kClipboardStreamMagic, 4) == 0) {
                const bool allowed =
                    role != Role::Server ||
                    (session_permissions.load() & kPermissionClipboard) != 0;
                if (!allowed || clipboard_streams.size() >= kMaxClipboardStreams ||
                    pending_clipboard_bytes + pending->size() > kMaxPendingClipboardBytes) {
                    kind = Kind::Bad;
                } else {
                    ClipboardStream entry;
                    entry.stream = stream;
                    entry.bytes = std::move(*pending);
                    pending_clipboard_bytes += entry.bytes.size();
                    clipboard_streams.push_back(std::move(entry));
                    kind = Kind::Clipboard;
                }
            } else if (std::memcmp(pending->data(), kFileStreamMagic, 4) == 0) {
                // Either side may send; the permission bit says whether the
                // session carries file transfers at all.
                const bool allowed =
                    (session_permissions.load() & kPermissionFileTransfer) != 0;
                if (!allowed) {
                    kind = Kind::Bad;
                } else if (pending->size() < kFileStreamHeaderSize) {
                    // The fixed prefix has not arrived yet.
                    return;
                } else {
                    FileStreamHeader header;
                    std::string error;
                    // The first receive usually carries the header and the
                    // first chunk together; decode only the fixed prefix.
                    if (!decode_file_stream_header(pending->data(), kFileStreamHeaderSize,
                                                   header, &error) ||
                        received_uploads.count(header.transfer_id) != 0 ||
                        received_uploads.size() >= kMaxIncomingUploads ||
                        header.offset > kMaxFileTransferBytes) {
                        kind = Kind::Bad;
                    } else {
                        ReceivedUpload upload;
                        upload.stream = stream;
                        upload.start_offset = header.offset;
                        const auto inserted =
                            received_uploads.emplace(header.transfer_id, std::move(upload));
                        if (pending->size() > kFileStreamHeaderSize) {
                            push_file_chunk_locked(
                                inserted.first->second, inserted.first->first,
                                pending->data() + kFileStreamHeaderSize,
                                pending->size() - kFileStreamHeaderSize, header.offset);
                        }
                        kind = Kind::File;
                    }
                }
            } else {
                kind = Kind::Bad;
            }
            if (kind != Kind::Wait) {
                unclassified.erase(
                    std::remove_if(unclassified.begin(), unclassified.end(),
                                   [stream](const std::pair<HQUIC, std::vector<uint8_t>>& entry) {
                                       return entry.first == stream;
                                   }),
                    unclassified.end());
            }
        }
        if (kind == Kind::Reject) {
            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        } else if (kind == Kind::Bad) {
            note_transport_failure("the peer opened an unusable stream");
        }
        cv.notify_all();
    }

    // Caller holds `mutex` and an entry in received_uploads. Drops every queued
    // byte of the transfer when the receive window would overflow and marks
    // the upload as broken; the host then reports IoError.
    void push_file_chunk_locked(ReceivedUpload& upload, uint32_t id, const uint8_t* data,
                                size_t size, uint64_t offset) {
        if (upload.finished) {
            return;
        }
        if (buffered_file_bytes + size > kMaxBufferedFileBytes) {
            upload.finished = true;
            upload.error = "the receive queue overflowed";
            drop_file_chunks_locked(id);
            if (upload.stream) {
                api->StreamShutdown(upload.stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            }
            return;
        }
        FileChunk chunk;
        chunk.transfer_id = id;
        chunk.offset = offset;
        chunk.data.assign(data, data + size);
        upload.received += size;
        buffered_file_bytes += size;
        file_chunks.push_back(std::move(chunk));
    }

    void drop_file_chunks_locked(uint32_t id) {
        for (auto it = file_chunks.begin(); it != file_chunks.end();) {
            if (it->transfer_id == id) {
                buffered_file_bytes -= it->data.size();
                it = file_chunks.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool has_active_upload_locked() const {
        for (const auto& entry : received_uploads) {
            if (!entry.second.finished) {
                return true;
            }
        }
        return false;
    }

    // Caller holds `mutex`.
    bool is_microphone_handle_locked(HQUIC stream) const {
        return std::find(microphone_handles.begin(), microphone_handles.end(), stream) !=
               microphone_handles.end();
    }

    // Appends data of a stream that was already classified as CCLB or CFIL.
    // Returns false when the handle belongs to neither, so the caller can
    // classify it from its first bytes.
    bool route_bulk_stream(HQUIC stream, QUIC_STREAM_EVENT& event, HQUIC* pause) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (HQUIC handle : microphone_handles) {
                if (handle == stream && stream != microphone.load()) {
                    // A replaced CMIC stream may still deliver buffered bytes;
                    // ownership of the receive window stays with MsQuic, so
                    // the event is consumed and forgotten.
                    return true;
                }
            }
            for (auto& entry : clipboard_streams) {
                if (entry.stream != stream) {
                    continue;
                }
                if (!entry.finished) {
                    for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                        const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                        // Dropping one oversized blob must not end the session:
                        // abort this stream only and keep the last item.
                        if (entry.bytes.size() + buffer.Length > kMaxClipboardStreamBytes) {
                            pending_clipboard_bytes -= entry.bytes.size();
                            entry.bytes.clear();
                            entry.finished = true;
                            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                            break;
                        }
                        entry.bytes.insert(entry.bytes.end(), buffer.Buffer,
                                           buffer.Buffer + buffer.Length);
                        pending_clipboard_bytes += buffer.Length;
                    }
                }
                return true;
            }
            for (auto& entry : received_uploads) {
                if (entry.second.stream != stream) {
                    continue;
                }
                // Consuming zero bytes leaves them with MsQuic, which then
                // withholds credit from the viewer until the host drains the
                // queue and re-enables this stream.
                if (!entry.second.finished &&
                    (entry.second.receive_paused ||
                     buffered_file_bytes >= kFileReceiveHighWaterBytes ||
                     buffered_file_bytes + event.RECEIVE.TotalBufferLength >
                         kMaxBufferedFileBytes)) {
                    entry.second.receive_paused = true;
                    event.RECEIVE.TotalBufferLength = 0;
                    *pause = stream;
                    return true;
                }
                for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                    push_file_chunk_locked(
                        entry.second, entry.first, buffer.Buffer, buffer.Length,
                        entry.second.start_offset + entry.second.received);
                }
                return true;
            }
        }
        return false;
    }

    // Caller holds `mutex`. Hands back every stream handle this side still owns
    // for clipboard/file traffic and forgets all of it, so a session reset
    // cannot leak a stream or carry a half-received item into the next one.
    std::vector<HQUIC> detach_bulk_streams_locked() {
        std::vector<HQUIC> handles;
        for (auto& entry : clipboard_streams) {
            if (entry.stream) {
                handles.push_back(entry.stream);
            }
        }
        for (auto& entry : received_uploads) {
            if (entry.second.stream) {
                handles.push_back(entry.second.stream);
            }
        }
        for (auto& entry : outgoing_uploads) {
            if (entry.second.stream) {
                handles.push_back(entry.second.stream);
            }
        }
        handles.insert(handles.end(), one_shot_streams.begin(), one_shot_streams.end());
        clipboard_streams.clear();
        pending_clipboard_bytes = 0;
        clipboard_contents.clear();
        clipboard_contents_seen = 0;
        file_offers.clear();
        file_offers_seen = 0;
        file_chunks.clear();
        buffered_file_bytes = 0;
        received_uploads.clear();
        file_cancels.clear();
        outgoing_uploads.clear();
        file_answers.clear();
        file_results.clear();
        one_shot_streams.clear();
        return handles;
    }

    // Caller holds `mutex`. Same for the microphone stream: the handles go
    // back to the caller for closing and every trace of the uplink is gone,
    // so a session reset never leaks a handle or a half-parsed packet.
    std::vector<HQUIC> detach_microphone_locked() {
        std::vector<HQUIC> handles = std::move(microphone_handles);
        microphone_handles.clear();
        microphone = nullptr;
        microphone_finished = false;
        microphone_info = AudioInfo{};
        microphone_format_changed = false;
        microphone_received.clear();
        microphone_parser.reset();
        return handles;
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
                HQUIC pause_stream = nullptr;
                if (stream == video.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    copy_chunks(received, event);
                    cv.notify_all();
                } else if (stream == audio.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    copy_chunks(audio_received, event);
                    cv.notify_all();
                } else if (stream == microphone.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    copy_chunks(microphone_received, event);
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
                } else if (!route_bulk_stream(stream, event, &pause_stream)) {
                    classify_media_stream(stream, event);
                }
                if (pause_stream) {
                    api->StreamReceiveSetEnabled(pause_stream, FALSE);
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
                    } else if (stream == microphone.load()) {
                        microphone_finished = true;
                    } else if (stream == control.load()) {
                        control_finished = true;
                    } else {
                        bool matched = false;
                        for (auto& entry : clipboard_streams) {
                            if (entry.stream != stream) {
                                continue;
                            }
                            entry.finished = true;
                            matched = true;
                            const bool allowed =
                                role != Role::Server ||
                                (session_permissions.load() & kPermissionClipboard) != 0;
                            if (allowed) {
                                ClipboardContent content;
                                std::string error;
                                // A malformed or over-limit blob is dropped,
                                // not fatal: the session and the last
                                // clipboard item survive it.
                                if (decode_clipboard_stream(entry.bytes.data(), entry.bytes.size(),
                                                            content, &error)) {
                                    // Clipboard means "the current item"; a
                                    // slow consumer wants the newest, so the
                                    // queue is short and drops the oldest.
                                    while (clipboard_contents.size() >= 4) {
                                        clipboard_contents.pop_front();
                                    }
                                    clipboard_contents.push_back(std::move(content));
                                    ++clipboard_contents_seen;
                                }
                            }
                            pending_clipboard_bytes -= entry.bytes.size();
                            entry.bytes.clear();
                            break;
                        }
                        if (!matched) {
                            for (auto& entry : received_uploads) {
                                if (entry.second.stream == stream) {
                                    entry.second.finished = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                cv.notify_all();
                break;
            }
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED: {
                bool fatal = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    bool bulk = false;
                    for (auto& entry : clipboard_streams) {
                        if (entry.stream == stream) {
                            entry.finished = true;
                            pending_clipboard_bytes -= entry.bytes.size();
                            entry.bytes.clear();
                            bulk = true;
                            break;
                        }
                    }
                    if (!bulk) {
                        for (auto& entry : received_uploads) {
                            if (entry.second.stream == stream) {
                                entry.second.finished = true;
                                entry.second.error = "the viewer aborted the upload";
                                drop_file_chunks_locked(entry.first);
                                bulk = true;
                                break;
                            }
                        }
                    }
                    if (!bulk) {
                        for (auto& entry : outgoing_uploads) {
                            if (entry.second.stream == stream) {
                                entry.second.finished = true;
                                bulk = true;
                                break;
                            }
                        }
                    }
                    if (stream == video.load()) {
                        video_finished = true;
                        fatal = !bulk;
                    } else if (stream == audio.load()) {
                        // Audio is optional: a broken audio stream ends audio,
                        // not the video session.
                        audio_finished = true;
                    } else if (stream == microphone.load() ||
                               is_microphone_handle_locked(stream)) {
                        // Same for the microphone; a retired handle only needs
                        // its completion event, which is not an error.
                        if (stream == microphone.load()) {
                            microphone_finished = true;
                        }
                    } else if (stream == control.load()) {
                        fatal = true;
                    } else if (!bulk) {
                        fatal = true;
                    }
                }
                if (fatal) {
                    note_transport_failure("QUIC peer aborted a stream");
                }
                cv.notify_all();
                break;
            }
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
                std::vector<HQUIC> to_close;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stream == video.load()) {
                        video_shutdown_complete = true;
                    } else if (stream == audio.load()) {
                        audio_shutdown_complete = true;
                    } else if (stream == control.load()) {
                        control_shutdown_complete = true;
                    } else {
                        bool microphone_stream = false;
                        for (auto it = microphone_handles.begin();
                             it != microphone_handles.end(); ++it) {
                            if (*it == stream) {
                                microphone_handles.erase(it);
                                microphone_stream = true;
                                break;
                            }
                        }
                        if (microphone_stream) {
                            if (stream == microphone.load()) {
                                microphone = nullptr;
                                microphone_finished = true;
                            }
                            to_close.push_back(stream);
                        } else {
                            for (auto it = clipboard_streams.begin();
                                 it != clipboard_streams.end(); ++it) {
                                if (it->stream == stream) {
                                    pending_clipboard_bytes -= it->bytes.size();
                                    it->bytes.clear();
                                    clipboard_streams.erase(it);
                                    to_close.push_back(stream);
                                    break;
                                }
                            }
                            for (auto& entry : received_uploads) {
                                if (entry.second.stream == stream) {
                                    entry.second.stream = nullptr;
                                    entry.second.finished = true;
                                    to_close.push_back(stream);
                                    break;
                                }
                            }
                            for (auto& entry : outgoing_uploads) {
                                if (entry.second.stream == stream) {
                                    entry.second.stream = nullptr;
                                    entry.second.finished = true;
                                    to_close.push_back(stream);
                                    break;
                                }
                            }
                            for (auto it = one_shot_streams.begin();
                                 it != one_shot_streams.end(); ++it) {
                                if (*it == stream) {
                                    one_shot_streams.erase(it);
                                    to_close.push_back(stream);
                                    break;
                                }
                            }
                            // A stream rejected during classification (bad
                            // magic, malformed microphone header) owns no
                            // state here but still holds its handle. The
                            // loops above already queued known streams.
                            if (std::find(to_close.begin(), to_close.end(), stream) ==
                                to_close.end()) {
                                to_close.push_back(stream);
                            }
                        }
                    }
                }
                for (HQUIC handle : to_close) {
                    api->StreamClose(handle);
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
                ClientHello hello;
                if (!parse_hello(payload, hello, &error)) {
                    note_transport_failure("Hello: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    requested_display = hello.display;
                    requested_peer = remote_address();
                    pending_hello = std::move(hello);
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
                    session_permissions = permission_mask_from_info(info);
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
                    session_permissions = permission_mask_from_info(info);
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
                if (role != Role::Server ||
                    (session_permissions.load() & kPermissionInput) == 0) {
                    // The HelloAck already told the viewer input is off; a
                    // client that sends it anyway is dropped, not fatal. This
                    // also covers a mid-session revocation: the operator does
                    // not have to trust the viewer to stop by itself.
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
                // Processed even when input is revoked: this message only
                // lifts what is held, and a viewer that lost input before its
                // key-up still has to be able to release it.
                if (role != Role::Server) {
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
            case ControlType::PermissionUpdate: {
                if (role != Role::Client) {
                    break;
                }
                uint16_t permissions = 0;
                if (!parse_permission_update(payload, permissions, &error)) {
                    note_transport_failure("PermissionUpdate: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_permissions = permissions;
                    permission_update_ready = true;
                    ++permission_updates;
                    if ((permissions & kPermissionClipboard) == 0) {
                        clipboard_contents.clear();
                    }
                    if ((permissions & kPermissionFileTransfer) == 0) {
                        for (auto& entry : outgoing_uploads) {
                            if (!entry.second.finished) {
                                entry.second.finished = true;
                                if (entry.second.stream) {
                                    api->StreamShutdown(entry.second.stream,
                                                        QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                                }
                            }
                        }
                    }
                }
                cv.notify_all();
                break;
            }
            case ControlType::SwitchDisplay: {
                if (role != Role::Server ||
                    (session_permissions.load() & kPermissionInput) == 0) {
                    // Switching outputs changes what the viewer sees, so it
                    // travels with input control; a viewer without it is
                    // dropped rather than allowed to move the host's view.
                    break;
                }
                uint16_t index = kDisplayAuto;
                if (!parse_switch_display(payload, index, &error)) {
                    note_transport_failure("SwitchDisplay: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_switch_index = index;
                    display_switch_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplaySwitchResult: {
                if (role != Role::Client) {
                    break;
                }
                uint16_t index = 0;
                DisplaySwitchCode code = DisplaySwitchCode::Failed;
                std::string reason;
                if (!parse_display_switch_result(payload, index, code, reason, &error)) {
                    note_transport_failure("DisplaySwitchResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_switch_result_index = index;
                    display_switch_code = code;
                    display_switch_reason = std::move(reason);
                    display_switch_result_ready = true;
                    ++display_switch_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::ListDisplayModes: {
                if (role != Role::Server) {
                    break;
                }
                uint16_t index = 0;
                if (!parse_list_display_modes(payload, index, &error)) {
                    note_transport_failure("ListDisplayModes: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_modes_request_display = index;
                    display_modes_request_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplayModes: {
                if (role != Role::Client) {
                    break;
                }
                DisplayModeList list;
                if (!parse_display_modes(payload, list, &error)) {
                    note_transport_failure("DisplayModes: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_modes = std::move(list);
                    display_modes_ready = true;
                    ++display_mode_lists_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SetDisplayMode: {
                if (role != Role::Server ||
                    (session_permissions.load() & kPermissionInput) == 0) {
                    // Changing the host's mode affects the operator's desktop,
                    // so it travels with input control like SwitchDisplay.
                    break;
                }
                uint16_t index = 0;
                uint16_t width = 0;
                uint16_t height = 0;
                uint16_t refresh_hz = 0;
                if (!parse_set_display_mode(payload, index, width, height, refresh_hz,
                                            &error)) {
                    note_transport_failure("SetDisplayMode: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    set_display_mode_index = index;
                    set_display_mode_width = width;
                    set_display_mode_height = height;
                    set_display_mode_refresh = refresh_hz;
                    set_display_mode_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplayModeResult: {
                if (role != Role::Client) {
                    break;
                }
                uint16_t index = 0;
                uint16_t width = 0;
                uint16_t height = 0;
                uint16_t refresh_hz = 0;
                DisplayModeCode code = DisplayModeCode::Failed;
                std::string reason;
                if (!parse_display_mode_result(payload, index, width, height, refresh_hz,
                                               code, reason, &error)) {
                    note_transport_failure("DisplayModeResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_mode_result_index = index;
                    display_mode_result_width = width;
                    display_mode_result_height = height;
                    display_mode_result_refresh = refresh_hz;
                    display_mode_code = code;
                    display_mode_reason = std::move(reason);
                    display_mode_result_ready = true;
                    ++display_mode_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::ClipboardUpdate: {
                if (role == Role::Server &&
                    (session_permissions.load() & kPermissionClipboard) == 0) {
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
            case ControlType::FileOffer: {
                if ((session_permissions.load() & kPermissionFileTransfer) == 0) {
                    break;
                }
                FileOffer offer;
                if (!parse_file_offer(payload, offer, &error)) {
                    note_transport_failure("FileOffer: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // The sender waits for an answer; dropping an offer past
                    // the queue is a refusal by silence only when the queue
                    // never drains, which the receiver prevents.
                    if (file_offers.size() >= kMaxQueuedOffers) {
                        break;
                    }
                    file_offers.push_back(std::move(offer));
                    ++file_offers_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileAnswer: {
                FileAnswer answer;
                if (!parse_file_answer(payload, answer, &error)) {
                    note_transport_failure("FileAnswer: " + error);
                    break;
                }
                bool bad_offset = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto it = outgoing_uploads.find(answer.transfer_id);
                    if (it != outgoing_uploads.end()) {
                        if (answer.code == FileCode::Ok) {
                            if (answer.resume_offset <= it->second.offer.size) {
                                it->second.accepted = true;
                                it->second.next_offset = answer.resume_offset;
                                file_answers.push_back(std::move(answer));
                            } else {
                                bad_offset = true;
                            }
                        } else {
                            file_answers.push_back(std::move(answer));
                        }
                    }
                }
                if (bad_offset) {
                    note_transport_failure("FileAnswer: resume offset past the file size");
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileResult: {
                FileResult result;
                if (!parse_file_result(payload, result, &error)) {
                    note_transport_failure("FileResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto it = outgoing_uploads.find(result.transfer_id);
                    if (it != outgoing_uploads.end()) {
                        it->second.finished = true;
                        // The stream may still be open when the host answers
                        // early; the handle is closed when the result is taken.
                        if (it->second.stream) {
                            api->StreamShutdown(it->second.stream,
                                                QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                        }
                    }
                    while (file_results.size() >= 8) {
                        file_results.pop_front();
                    }
                    file_results.push_back(std::move(result));
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileCancel: {
                FileCancel cancel;
                if (!parse_file_cancel(payload, cancel, &error)) {
                    note_transport_failure("FileCancel: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto it = received_uploads.find(cancel.transfer_id);
                    if (it != received_uploads.end()) {
                        it->second.finished = true;
                        it->second.error = "the peer cancelled the transfer";
                        drop_file_chunks_locked(cancel.transfer_id);
                        if (it->second.stream) {
                            api->StreamShutdown(it->second.stream,
                                                QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                        }
                    }
                    while (file_cancels.size() >= 8) {
                        file_cancels.pop_front();
                    }
                    file_cancels.push_back(std::move(cancel));
                }
                cv.notify_all();
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

    // A non-blocking capacity probe. The caller checks before touching any
    // stream state, so a full queue never leaves a half-started upload.
    bool has_send_capacity(size_t bytes) {
        std::lock_guard<std::mutex> lock(send_mutex);
        return in_flight_sends < kMaxInFlightSends &&
               in_flight_bytes + bytes <= kMaxInFlightBytes;
    }

    // A FIN with no payload; MsQuic allows a zero-length send.
    Error send_stream_fin(HQUIC stream) {
        if (!stream) {
            return Error(Status::Unavailable, "no stream is open");
        }
        {
            std::unique_lock<std::mutex> lock(send_mutex);
            send_cv.wait(lock, [this] {
                return (in_flight_sends < kMaxInFlightSends &&
                        in_flight_bytes < kMaxInFlightBytes) ||
                       transport_down.load();
            });
            if (transport_down.load()) {
                return Error(Status::Unavailable, "connection closed before send");
            }
            ++in_flight_sends;
        }
        auto* pending = new PendingSend;
        static uint8_t empty = 0;
        pending->buffer.Buffer = &empty;
        pending->buffer.Length = 0;
        const QUIC_STATUS status =
            api->StreamSend(stream, &pending->buffer, 1, QUIC_SEND_FLAG_FIN, pending);
        if (QUIC_FAILED(status)) {
            release_send(0);
            delete pending;
            if (status == QUIC_STATUS_ABORTED || status == QUIC_STATUS_INVALID_STATE) {
                return Error(Status::Unavailable, "the connection closed before the send");
            }
            return quic_error(status, "QUIC StreamSend(FIN)");
        }
        return Error::success();
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
        std::vector<HQUIC> bulk;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& entry : unclassified) {
                bulk.push_back(entry.first);
            }
            unclassified.clear();
            std::vector<HQUIC> more = detach_bulk_streams_locked();
            bulk.insert(bulk.end(), more.begin(), more.end());
            std::vector<HQUIC> microphones = detach_microphone_locked();
            bulk.insert(bulk.end(), microphones.begin(), microphones.end());
        }
        for (HQUIC handle : bulk) {
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
            pending_hello = ClientHello{};
            requested_display = kDisplayAuto;
            requested_peer.clear();
            display_switch_ready = false;
            display_switch_index = kDisplayAuto;
            display_switch_result_ready = false;
            display_switch_result_index = 0;
            display_switch_code = DisplaySwitchCode::Ok;
            display_switch_reason.clear();
            display_switch_results_seen = 0;
            display_modes_request_ready = false;
            display_modes_request_display = 0;
            display_modes_ready = false;
            display_modes = DisplayModeList{};
            display_mode_lists_seen = 0;
            set_display_mode_ready = false;
            set_display_mode_index = 0;
            set_display_mode_width = 0;
            set_display_mode_height = 0;
            set_display_mode_refresh = 0;
            display_mode_result_ready = false;
            display_mode_result_index = 0;
            display_mode_result_width = 0;
            display_mode_result_height = 0;
            display_mode_result_refresh = 0;
            display_mode_code = DisplayModeCode::Ok;
            display_mode_reason.clear();
            display_mode_results_seen = 0;
            session_permissions = 0;
            permission_ceiling = 0;
            permission_update_ready = false;
            permission_updates = 0;
            hello_complete = false;
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
            microphone_streams_seen = 0;
            unclassified.clear();
            transport_failed = false;
            transport_status = Status::Ok;
            transport_message.clear();
        }
        transport_down = false;
        next_audio_seq = 0;
        next_microphone_seq = 0;
        // Only now can the listener claim the slot for the next client.
        connection = nullptr;
        cv.notify_all();
        send_cv.notify_all();
    }

    // The hard gates from QuicServerConfig. Authentication and approval may
    // only reduce this; the transport never grants beyond it.
    uint16_t config_permissions() const {
        uint16_t mask = 0;
        if (server_config.allow_input) {
            mask |= kPermissionInput;
        }
        if (server_config.allow_clipboard) {
            mask |= kPermissionClipboard;
        }
        if (server_config.allow_file_transfer) {
            mask |= kPermissionFileTransfer;
        }
        if (server_config.allow_audio) {
            mask |= kPermissionAudio;
        }
        if (server_config.allow_microphone) {
            mask |= kPermissionMicrophone;
        }
        return mask;
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
            std::vector<HQUIC> more = detach_bulk_streams_locked();
            parked.insert(parked.end(), more.begin(), more.end());
            std::vector<HQUIC> microphones = detach_microphone_locked();
            parked.insert(parked.end(), microphones.begin(), microphones.end());
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
    // The client may open one bidirectional stream: the control channel. Its
    // unidirectional streams carry rich clipboard blobs (at most
    // kMaxClipboardStreams at once), one CFIL upload and the CMIC microphone
    // stream; the cap leaves room for all of them plus the streams MsQuic
    // keeps until their FIN is acknowledged.
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerUnidiStreamCount = static_cast<uint16_t>(
        kMaxClipboardStreams + kMaxIncomingUploads + 2);
    settings.IsSet.PeerUnidiStreamCount = TRUE;
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
    // system audio, a second for audio. Its rich clipboard blobs use more, so
    // the cap is the receive-side stream budget rather than two.
    settings.PeerUnidiStreamCount = static_cast<uint16_t>(kMaxClipboardStreams + 2);
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
    const uint16_t ceiling = impl.config_permissions();
    for (;;) {
        std::unique_lock<std::mutex> lock(impl.mutex);
        const bool woken = impl.cv.wait_until(lock, deadline, [&impl] {
            return impl.hello_ready || impl.transport_failed ||
                   impl.shutdown_complete || impl.peer_closed;
        });
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.hello_ready) {
            // Consumed here: the next wait must be triggered by a new Hello,
            // not by the one this call already answered.
            impl.hello_ready = false;
            ClientRequest request;
            request.display = impl.requested_display;
            request.peer_address = impl.requested_peer;
            request.hello = std::move(impl.pending_hello);
            impl.pending_hello = ClientHello{};
            request.peer_alive = [&impl] { return impl.peer_alive(); };
            lock.unlock();

            ClientAuthResult auth;
            std::string refusal;
            auto refuse = [&](AccessDenial code, const std::string& reason) {
                refusal = reason;
                impl.refuse_client(code, reason);
            };
            if (impl.server_config.authenticate) {
                auth = impl.server_config.authenticate(request);
                if (auth.action == ClientAuthAction::Deny) {
                    refuse(auth.denial, auth.reason.empty() ? "the host refused the connection"
                                                            : auth.reason);
                }
                request.permissions = auth.permissions & ceiling;
            } else {
                // Legacy gate: the shared token, then the boolean approval.
                std::string expected;
                bool used = false;
                bool reusable = false;
                {
                    std::lock_guard<std::mutex> guard(impl.mutex);
                    expected = impl.server_config.access_token;
                    used = impl.token_consumed;
                    reusable = impl.server_config.reuse_access_token;
                }
                if (!expected.empty() && !tokens_match(request.hello.access_token, expected)) {
                    refuse(AccessDenial::InvalidToken, "the access token was not accepted");
                } else if (!expected.empty() && used && !reusable) {
                    refuse(AccessDenial::TokenUsed, "the one-time access token was already used");
                } else {
                    request.permissions = ceiling;
                    auth.action = impl.server_config.approve_client
                                      ? ClientAuthAction::RequireApproval
                                      : ClientAuthAction::Allow;
                    auth.permissions = ceiling;
                }
            }

            if (refusal.empty() && auth.action == ClientAuthAction::RequireApproval) {
                if (!impl.server_config.approve_client) {
                    refuse(AccessDenial::Declined,
                           "the host requires approval but cannot ask for it");
                } else {
                    const ClientApproval approval = impl.server_config.approve_client(request);
                    if (!approval.allow) {
                        refuse(AccessDenial::Declined, "the host declined the connection");
                    } else {
                        auth.action = ClientAuthAction::Allow;
                        auth.permissions = request.permissions & approval.permissions;
                    }
                }
            }

            if (refusal.empty() && !impl.peer_alive()) {
                // The viewer can give up while an operator prompt is open (the
                // command-line handshake timeout is 15 s). Admitting it then
                // would announce into a connection that is already gone and
                // fail with E_ABORT; treat it as an expired handshake instead.
                refuse(AccessDenial::AuthTimeout,
                       "the viewer stopped waiting for approval");
            }

            if (refusal.empty()) {
                const uint16_t granted = auth.permissions & ceiling;
                impl.session_permissions = granted;
                impl.permission_ceiling = granted;
                if (!impl.server_config.authenticate) {
                    impl.consume_token();
                }
                return Error::success();
            }
            if (impl.server_config.stop_after_denial) {
                return Error(Status::PermissionDenied, refusal);
            }
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

uint16_t QuicSession::permissions() const {
    return impl_->session_permissions.load();
}

Error QuicSession::set_permissions(uint16_t permissions) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "set_permissions is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream was not announced yet");
    }
    if (!impl.server_config.allow_permission_change) {
        return Error(Status::InvalidCall, "permission changes are disabled on this host");
    }
    uint16_t next = 0;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        next = permissions & impl.permission_ceiling & impl.config_permissions();
        if (next == impl.session_permissions.load()) {
            return Error::success();
        }
        // The local gate changes before the message goes out: if telling the
        // viewer fails, the host is still the one enforcing the reduction.
        impl.session_permissions = next;
    }
    if ((next & kPermissionInput) == 0 && impl.injector) {
        impl.injector->release_held();
    }
    if ((next & kPermissionMicrophone) == 0) {
        // The uplink is viewer-initiated, so revoking it means aborting the
        // stream: the viewer sees the reset and stops capturing. A new stream
        // after a later grant goes through classification again.
        HQUIC handle = impl.microphone.exchange(nullptr);
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.microphone_finished = true;
            impl.microphone_received.clear();
            impl.microphone_parser.reset();
            impl.microphone_info = AudioInfo{};
            impl.microphone_format_changed = false;
        }
        if (handle) {
            impl.api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        }
    }
    if ((next & kPermissionClipboard) == 0) {
        std::vector<HQUIC> aborts;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            for (const auto& entry : impl.clipboard_streams) {
                if (entry.stream) {
                    aborts.push_back(entry.stream);
                }
            }
            impl.clipboard_streams.clear();
            impl.pending_clipboard_bytes = 0;
            impl.clipboard_contents.clear();
        }
        for (HQUIC handle : aborts) {
            impl.api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        }
    }
    if ((next & kPermissionFileTransfer) == 0) {
        std::vector<std::pair<uint32_t, uint64_t>> cancelled;
        std::vector<HQUIC> aborts;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            for (auto& entry : impl.received_uploads) {
                if (!entry.second.finished) {
                    cancelled.emplace_back(entry.first, entry.second.received);
                    entry.second.finished = true;
                    entry.second.error = "file transfer was disabled";
                    if (entry.second.stream) {
                        aborts.push_back(entry.second.stream);
                    }
                }
            }
            impl.file_offers.clear();
            impl.file_chunks.clear();
            impl.buffered_file_bytes = 0;
        }
        for (HQUIC handle : aborts) {
            impl.api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        }
        for (const auto& entry : cancelled) {
            FileResult result;
            result.transfer_id = entry.first;
            result.code = FileCode::Cancelled;
            result.received = entry.second;
            result.reason = "file transfer was disabled";
            impl.try_send_bytes(impl.control.load(), make_file_result(result),
                                QUIC_SEND_FLAG_NONE);
        }
    }
    return impl.send_bytes(impl.control.load(), make_permission_update(next),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_permission_update(uint16_t& permissions) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->permission_update_ready) {
        return false;
    }
    permissions = impl_->session_permissions.load();
    impl_->permission_update_ready = false;
    return true;
}

uint64_t QuicSession::permission_updates() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->permission_updates;
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
    // session will not inject or audio the host disabled. The accepted mask
    // is already clamped to these gates, so it is the single source of truth
    // and includes any reduction the operator made at approval time.
    const uint16_t granted = impl.session_permissions.load();
    ack.input_enabled = (granted & kPermissionInput) != 0;
    ack.clipboard_enabled = (granted & kPermissionClipboard) != 0;
    ack.file_transfer_enabled = (granted & kPermissionFileTransfer) != 0;
    ack.microphone_enabled = (granted & kPermissionMicrophone) != 0;
    ack.audio.enabled = ack.audio.enabled && (granted & kPermissionAudio) != 0;
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
    // The server side of the handshake is complete once HelloAck is on the
    // wire. Later server-initiated messages (the operator's file offers) use
    // the same guard as the client's.
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.hello_complete = true;
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
    const uint16_t granted = impl.session_permissions.load();
    update.input_enabled = (granted & kPermissionInput) != 0;
    update.clipboard_enabled = (granted & kPermissionClipboard) != 0;
    update.file_transfer_enabled = (granted & kPermissionFileTransfer) != 0;
    update.microphone_enabled = (granted & kPermissionMicrophone) != 0;
    bool audio_stream_open = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        audio_stream_open = impl.audio.load() != nullptr && !impl.audio_stopped;
    }
    update.audio.enabled =
        update.audio.enabled && (granted & kPermissionAudio) != 0 && audio_stream_open;
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

Error QuicSession::request_display_switch(uint16_t display_index) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_display_switch is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.session_permissions.load() & kPermissionInput) == 0) {
            return Error(Status::PermissionDenied, "input is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(), make_switch_display(display_index),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_display_switch_result(uint16_t& display_index, DisplaySwitchCode& code,
                                             std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_switch_result_ready) {
        return false;
    }
    display_index = impl_->display_switch_result_index;
    code = impl_->display_switch_code;
    reason = impl_->display_switch_reason;
    impl_->display_switch_result_ready = false;
    return true;
}

uint64_t QuicSession::display_switch_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->display_switch_results_seen;
}

Error QuicSession::request_display_modes(uint16_t display_index) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_display_modes is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
    }
    return impl.send_bytes(impl.control.load(), make_list_display_modes(display_index),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_display_modes(DisplayModeList& list) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_modes_ready) {
        return false;
    }
    list = impl_->display_modes;
    impl_->display_modes_ready = false;
    return true;
}

uint64_t QuicSession::display_mode_lists() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->display_mode_lists_seen;
}

Error QuicSession::request_set_display_mode(uint16_t display_index, uint16_t width,
                                            uint16_t height, uint16_t refresh_hz) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_set_display_mode is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.session_permissions.load() & kPermissionInput) == 0) {
            return Error(Status::PermissionDenied, "input is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(),
                           make_set_display_mode(display_index, width, height, refresh_hz),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_display_mode_result(uint16_t& display_index, uint16_t& width,
                                           uint16_t& height, uint16_t& refresh_hz,
                                           DisplayModeCode& code, std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_mode_result_ready) {
        return false;
    }
    display_index = impl_->display_mode_result_index;
    width = impl_->display_mode_result_width;
    height = impl_->display_mode_result_height;
    refresh_hz = impl_->display_mode_result_refresh;
    code = impl_->display_mode_code;
    reason = impl_->display_mode_reason;
    impl_->display_mode_result_ready = false;
    return true;
}

uint64_t QuicSession::display_mode_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->display_mode_results_seen;
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

bool QuicSession::take_display_switch_request(uint16_t& display_index) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_switch_ready) {
        return false;
    }
    display_index = impl_->display_switch_index;
    impl_->display_switch_ready = false;
    return true;
}

Error QuicSession::send_display_switch_result(uint16_t display_index, DisplaySwitchCode code,
                                              const std::string& reason) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_display_switch_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(),
                           make_display_switch_result(display_index, code, reason),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_list_display_modes_request(uint16_t& display_index) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_modes_request_ready) {
        return false;
    }
    display_index = impl_->display_modes_request_display;
    impl_->display_modes_request_ready = false;
    return true;
}

Error QuicSession::send_display_modes(const DisplayModeList& list) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_display_modes is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(), make_display_modes(list),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_set_display_mode_request(uint16_t& display_index, uint16_t& width,
                                                uint16_t& height, uint16_t& refresh_hz) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->set_display_mode_ready) {
        return false;
    }
    display_index = impl_->set_display_mode_index;
    width = impl_->set_display_mode_width;
    height = impl_->set_display_mode_height;
    refresh_hz = impl_->set_display_mode_refresh;
    impl_->set_display_mode_ready = false;
    return true;
}

Error QuicSession::send_display_mode_result(uint16_t display_index, uint16_t width,
                                            uint16_t height, uint16_t refresh_hz,
                                            DisplayModeCode code, const std::string& reason) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_display_mode_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(),
                           make_display_mode_result(display_index, width, height, refresh_hz,
                                                    code, reason),
                           QUIC_SEND_FLAG_NONE);
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
    // The operator may revoke audio mid-session. The stream stays open so a
    // restore resumes immediately, but nothing leaves the host in between.
    if ((impl.session_permissions.load() & kPermissionAudio) == 0) {
        return Error(Status::PermissionDenied, "audio is disabled for this session");
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

    ClientHello hello;
    hello.display = display;
    hello.access_token = impl.client_config.access_token;
    hello.password = impl.client_config.password;
    hello.totp = impl.client_config.totp;
    hello.requested_permissions = impl.client_config.requested_permissions & kPermissionAll;
    if (impl.client_config.device_identity) {
        const DeviceIdentity& identity = *impl.client_config.device_identity;
        hello.device_key = identity.public_key();
        hello.device_name = truncate_utf8(identity.name(), kMaxDeviceNameLength);
        hello.device_platform = static_cast<uint8_t>(impl.client_config.device_platform);
        if (!secure_random(hello.nonce.data(), hello.nonce.size())) {
            return Error(Status::CaptureFailed, "cannot generate the Hello nonce");
        }
        hello.unix_seconds = static_cast<uint64_t>(std::time(nullptr));
        hello.auth_flags = kAuthFlagSigned;
        if (impl.client_config.trust_device) {
            hello.auth_flags |= kAuthFlagTrustDevice;
        }
        std::string fingerprint;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            fingerprint = impl.peer_fingerprint;
        }
        std::string sign_error;
        if (!identity.sign(hello_signature_message(fingerprint, hello), hello.signature,
                           &sign_error)) {
            return Error(Status::CaptureFailed, "cannot sign the Hello: " + sign_error);
        }
    } else if (impl.client_config.trust_device) {
        return Error(Status::InvalidCall, "trust_device needs a device identity");
    }
    Error sent = impl.send_bytes(control, make_hello(hello), QUIC_SEND_FLAG_NONE);
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
    // `lock` from the wait above is still held here.
    info = impl.peer_info;
    impl.hello_complete = true;
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

Error QuicSession::receive_microphone(AudioHeader& header, std::vector<uint8_t>& payload,
                                      AudioInfo& format, bool& format_changed,
                                      std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "receive_microphone is a server operation");
    }
    format_changed = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    for (;;) {
        while (!impl.microphone_received.empty()) {
            std::vector<uint8_t> chunk = std::move(impl.microphone_received.front());
            impl.microphone_received.pop_front();
            lock.unlock();
            std::string error;
            const bool ok = impl.microphone_parser.push(chunk.data(), chunk.size(), &error);
            lock.lock();
            if (!ok) {
                return Error(Status::CaptureFailed, "microphone stream: " + error);
            }
        }

        const uint8_t* microphone_payload = nullptr;
        std::string parse_error;
        switch (impl.microphone_parser.next(header, &microphone_payload, &parse_error)) {
            case AudioParser::Status::Packet: {
                payload.assign(microphone_payload, microphone_payload + header.payload_size);
                impl.microphone_parser.consume();
                format = impl.microphone_info;
                format_changed = impl.microphone_format_changed;
                impl.microphone_format_changed = false;
                return Error::success();
            }
            case AudioParser::Status::Error:
                return Error(Status::CaptureFailed, "microphone stream: " + parse_error);
            case AudioParser::Status::Incomplete:
                break;
        }

        if (impl.microphone_finished) {
            if (impl.microphone_parser.buffered() > 0) {
                return Error(Status::CaptureFailed, "microphone stream ended mid-packet");
            }
            return Error(Status::Unavailable, "the viewer finished the microphone stream");
        }
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.shutdown_complete) {
            return Error(Status::Unavailable, "the connection closed");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(Status::WaitTimeout, "no microphone audio arrived in time");
        }
        impl.cv.wait_until(lock, deadline);
    }
}

Error QuicSession::start_microphone(const AudioInfo& info) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "start_microphone is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.session_permissions.load() & kPermissionMicrophone) == 0) {
            return Error(Status::PermissionDenied, "the host did not grant the microphone");
        }
        if (impl.microphone.load() != nullptr) {
            return Error(Status::InvalidCall, "the microphone stream is already open");
        }
    }
    std::vector<uint8_t> header = make_microphone_stream_header(info);
    if (header.empty()) {
        return Error(Status::InvalidCall,
                     "the microphone format is not 48 kHz Opus with Opus config");
    }
    HQUIC stream = nullptr;
    QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &Impl::stream_callback, &impl, &stream);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC StreamOpen(microphone)");
    }
    impl.microphone = stream;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.microphone_handles.push_back(stream);
        impl.next_microphone_seq = 0;
    }
    status = impl.api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        impl.microphone = nullptr;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.microphone_handles.erase(
                std::remove(impl.microphone_handles.begin(), impl.microphone_handles.end(),
                            stream),
                impl.microphone_handles.end());
        }
        impl.api->StreamClose(stream);
        return quic_error(status, "QUIC StreamStart(microphone)");
    }
    Error sent = impl.send_bytes(stream, std::move(header), QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        impl.microphone = nullptr;
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    }
    return sent;
}

Error QuicSession::send_microphone(AudioHeader header, const uint8_t* payload, size_t size) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "send_microphone is a client operation");
    }
    // The host may revoke the microphone mid-session and abort the stream.
    // Report that as a permission failure so the viewer UI can explain it.
    if ((impl.session_permissions.load() & kPermissionMicrophone) == 0) {
        return Error(Status::PermissionDenied, "the host revoked the microphone");
    }
    HQUIC microphone = impl.microphone.load();
    if (!microphone) {
        return Error(Status::InvalidCall, "the microphone stream is not open");
    }
    if (!payload || size == 0 || size > kMaxAudioPayload) {
        return Error(Status::InvalidCall, "invalid microphone payload size");
    }
    header.seq = impl.next_microphone_seq.fetch_add(1);
    header.payload_size = static_cast<uint32_t>(size);
    header.payload_crc32 = crc32(payload, size);
    std::vector<uint8_t> bytes(kAudioHeaderSize + size);
    encode_audio_header(header, bytes.data());
    std::memcpy(bytes.data() + kAudioHeaderSize, payload, size);
    // Same rule as send_audio: a full queue drops this packet rather than
    // stalling the frame loop that drives video.
    return impl.send_bytes_impl(microphone, std::move(bytes), QUIC_SEND_FLAG_NONE, false);
}

Error QuicSession::stop_microphone() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "stop_microphone is a client operation");
    }
    HQUIC microphone = impl.microphone.exchange(nullptr);
    if (!microphone) {
        return Error::success();
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.microphone_received.clear();
        impl.microphone_parser.reset();
        impl.microphone_format_changed = false;
    }
    impl.api->StreamShutdown(microphone, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    return Error::success();
}

bool QuicSession::microphone_finished() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->microphone_finished;
}

uint64_t QuicSession::microphone_streams() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->microphone_streams_seen;
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
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
    }
    if ((impl.session_permissions.load() & kPermissionInput) == 0) {
        return Error(Status::PermissionDenied, "input is disabled by the host");
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
    if (impl.role == Impl::Role::Server) {
        if ((impl.session_permissions.load() & kPermissionClipboard) == 0) {
            return Error(Status::PermissionDenied, "clipboard is disabled for this session");
        }
    } else {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.session_permissions.load() & kPermissionClipboard) == 0) {
            return Error(Status::PermissionDenied, "clipboard is disabled by the host");
        }
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

Error QuicSession::send_clipboard_content(ClipboardContent content) {
    std::vector<uint8_t> bytes = encode_clipboard_stream(content);
    if (bytes.empty()) {
        return Error(Status::InvalidCall, "invalid clipboard content");
    }
    return send_clipboard_blob(std::move(bytes));
}

Error QuicSession::send_clipboard_blob(std::vector<uint8_t> blob) {
    Impl& impl = *impl_;
    if (impl.role == Impl::Role::Server) {
        if ((impl.session_permissions.load() & kPermissionClipboard) == 0) {
            return Error(Status::PermissionDenied, "clipboard is disabled for this session");
        }
    } else {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.session_permissions.load() & kPermissionClipboard) == 0) {
            return Error(Status::PermissionDenied, "clipboard is disabled by the host");
        }
    }
    if (blob.empty()) {
        return Error(Status::InvalidCall, "the clipboard blob is empty");
    }
    HQUIC stream = nullptr;
    const QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &Impl::stream_callback, &impl, &stream);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC StreamOpen(clipboard)");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.one_shot_streams.push_back(stream);
    }
    Error sent = impl.send_bytes(
        stream, std::move(blob),
        static_cast<QUIC_SEND_FLAGS>(QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN));
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = std::find(impl.one_shot_streams.begin(), impl.one_shot_streams.end(),
                                  stream);
        if (it != impl.one_shot_streams.end()) {
            impl.one_shot_streams.erase(it);
        }
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        impl.api->StreamClose(stream);
    }
    return sent;
}

bool QuicSession::take_clipboard_content(ClipboardContent& content) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->clipboard_contents.empty()) {
        return false;
    }
    content = std::move(impl_->clipboard_contents.front());
    impl_->clipboard_contents.pop_front();
    return true;
}

uint64_t QuicSession::clipboard_contents_received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->clipboard_contents_seen;
}

bool QuicSession::take_file_offer(FileOffer& offer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_offers.empty()) {
        return false;
    }
    offer = std::move(impl_->file_offers.front());
    impl_->file_offers.pop_front();
    return true;
}

uint64_t QuicSession::file_offers_received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->file_offers_seen;
}

Error QuicSession::send_file_answer(uint32_t transfer_id, FileCode code, uint64_t resume_offset,
                                    const std::string& reason) {
    Impl& impl = *impl_;
    if (code != FileCode::Ok && resume_offset != 0) {
        return Error(Status::InvalidCall, "only an accepted upload carries a resume offset");
    }
    if (code != FileCode::Ok) {
        HQUIC stream = nullptr;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            const auto it = impl.received_uploads.find(transfer_id);
            if (it != impl.received_uploads.end()) {
                stream = it->second.stream;
                impl.drop_file_chunks_locked(transfer_id);
                impl.received_uploads.erase(it);
            }
        }
        if (stream) {
            impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            impl.api->StreamClose(stream);
        }
    }
    FileAnswer answer;
    answer.transfer_id = transfer_id;
    answer.code = code;
    answer.resume_offset = resume_offset;
    answer.reason = reason;
    return impl.send_bytes(impl.control.load(), make_file_answer(answer), QUIC_SEND_FLAG_NONE);
}

Error QuicSession::receive_file_chunk(FileChunk& chunk, std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    HQUIC resume = nullptr;
    Error result = Error::success();
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        impl.cv.wait_for(lock, timeout, [&impl] {
            return !impl.file_chunks.empty() || impl.transport_down.load() ||
                   !impl.has_active_upload_locked();
        });
        if (!impl.file_chunks.empty()) {
            chunk = std::move(impl.file_chunks.front());
            impl.file_chunks.pop_front();
            impl.buffered_file_bytes -= chunk.data.size();
            if (impl.buffered_file_bytes <= kFileReceiveLowWaterBytes) {
                for (auto& entry : impl.received_uploads) {
                    if (entry.second.receive_paused) {
                        entry.second.receive_paused = false;
                        resume = entry.second.stream;
                        break;
                    }
                }
            }
        } else if (impl.transport_failed) {
            result = Error(impl.transport_status, impl.transport_message);
        } else if (!impl.has_active_upload_locked()) {
            result = Error(Status::Unavailable, "no file upload is active");
        } else {
            result = Error(Status::WaitTimeout, "no file data arrived in time");
        }
    }
    if (resume) {
        impl.api->StreamReceiveSetEnabled(resume, TRUE);
    }
    return result;
}

bool QuicSession::file_upload_state(uint32_t transfer_id, FileUploadState& state) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->received_uploads.find(transfer_id);
    if (it == impl_->received_uploads.end()) {
        return false;
    }
    state.resume_offset = it->second.start_offset;
    state.received = it->second.received;
    state.error = it->second.error;
    bool queued = false;
    for (const FileChunk& chunk : impl_->file_chunks) {
        if (chunk.transfer_id == transfer_id) {
            queued = true;
            break;
        }
    }
    state.finished = it->second.finished && !queued;
    return true;
}

Error QuicSession::send_file_result(uint32_t transfer_id, FileCode code, uint64_t received,
                                   const std::string& reason) {
    Impl& impl = *impl_;
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.received_uploads.find(transfer_id);
        if (it != impl.received_uploads.end()) {
            stream = it->second.stream;
            impl.drop_file_chunks_locked(transfer_id);
            impl.received_uploads.erase(it);
        }
    }
    if (stream) {
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        impl.api->StreamClose(stream);
    }
    FileResult result;
    result.transfer_id = transfer_id;
    result.code = code;
    result.received = received;
    result.reason = reason;
    return impl.send_bytes(impl.control.load(), make_file_result(result), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_file_cancel(FileCancel& cancel) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_cancels.empty()) {
        return false;
    }
    cancel = std::move(impl_->file_cancels.front());
    impl_->file_cancels.pop_front();
    return true;
}

Error QuicSession::offer_file(const FileOffer& offer) {
    Impl& impl = *impl_;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.session_permissions.load() & kPermissionFileTransfer) == 0) {
            return Error(Status::PermissionDenied, "file transfer is disabled by the host");
        }
        if (impl.outgoing_uploads.count(offer.transfer_id) != 0) {
            return Error(Status::InvalidCall, "the transfer id is already in use");
        }
        if (impl.outgoing_uploads.size() >= 8) {
            return Error(Status::WaitTimeout, "too many uploads are already open");
        }
    }
    std::vector<uint8_t> message = make_file_offer(offer);
    if (message.empty()) {
        return Error(Status::InvalidCall, "the offer is not encodable");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        Impl::OutgoingUpload upload;
        upload.offer = offer;
        impl.outgoing_uploads.emplace(offer.transfer_id, std::move(upload));
    }
    Error sent = impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.outgoing_uploads.erase(offer.transfer_id);
    }
    return sent;
}

bool QuicSession::take_file_answer(FileAnswer& answer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_answers.empty()) {
        return false;
    }
    answer = std::move(impl_->file_answers.front());
    impl_->file_answers.pop_front();
    if (answer.code != FileCode::Ok) {
        const auto it = impl_->outgoing_uploads.find(answer.transfer_id);
        if (it != impl_->outgoing_uploads.end()) {
            it->second.finished = true;
        }
    }
    return true;
}

Error QuicSession::send_file_data(uint32_t transfer_id, const uint8_t* data, size_t size) {
    Impl& impl = *impl_;
    if (!data || size == 0 || size > kFileChunkBytes) {
        return Error(Status::InvalidCall, "a file chunk is between 1 byte and 64 KiB");
    }
    HQUIC stream = nullptr;
    std::vector<uint8_t> bytes;
    bool starting = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.outgoing_uploads.find(transfer_id);
        if (it == impl.outgoing_uploads.end() || !it->second.accepted ||
            it->second.finished) {
            return Error(Status::InvalidCall, "the upload was not accepted");
        }
        Impl::OutgoingUpload& upload = it->second;
        if (!upload.started) {
            QUIC_STATUS status = impl.api->StreamOpen(
                impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                &Impl::stream_callback, &impl, &upload.stream);
            if (QUIC_FAILED(status)) {
                return quic_error(status, "QUIC StreamOpen(upload)");
            }
            upload.started = true;
            starting = true;
            FileStreamHeader header;
            header.transfer_id = transfer_id;
            header.offset = upload.next_offset;
            bytes.resize(kFileStreamHeaderSize + size);
            encode_file_stream_header(header, bytes.data());
            std::memcpy(bytes.data() + kFileStreamHeaderSize, data, size);
        } else {
            bytes.assign(data, data + size);
        }
        stream = upload.stream;
        upload.next_offset += size;
    }
    Error sent = impl.send_bytes(stream, std::move(bytes),
                                 starting ? QUIC_SEND_FLAG_START : QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        HQUIC handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            const auto it = impl.outgoing_uploads.find(transfer_id);
            if (it != impl.outgoing_uploads.end()) {
                handle = it->second.stream;
                it->second.stream = nullptr;
                it->second.finished = true;
                impl.outgoing_uploads.erase(it);
            }
        }
        if (handle) {
            impl.api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            impl.api->StreamClose(handle);
        }
    }
    return sent;
}

Error QuicSession::try_send_file_data(uint32_t transfer_id, const uint8_t* data, size_t size) {
    Impl& impl = *impl_;
    if (!data || size == 0 || size > kFileChunkBytes) {
        return Error(Status::InvalidCall, "a file chunk is between 1 byte and 64 KiB");
    }
    if (!impl.has_send_capacity(size + kFileStreamHeaderSize)) {
        return Error(Status::WaitTimeout, "send queue is full");
    }
    return send_file_data(transfer_id, data, size);
}

Error QuicSession::finish_file_upload(uint32_t transfer_id) {
    Impl& impl = *impl_;
    HQUIC stream = nullptr;
    bool starting = false;
    std::vector<uint8_t> header_bytes;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.outgoing_uploads.find(transfer_id);
        if (it == impl.outgoing_uploads.end() || !it->second.accepted ||
            it->second.finished) {
            return Error(Status::InvalidCall, "the upload was not accepted");
        }
        Impl::OutgoingUpload& upload = it->second;
        if (!upload.started) {
            // A zero-byte file never called send_file_data(); the stream still
            // needs its header so the host can match it to the offer.
            QUIC_STATUS status = impl.api->StreamOpen(
                impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                &Impl::stream_callback, &impl, &upload.stream);
            if (QUIC_FAILED(status)) {
                return quic_error(status, "QUIC StreamOpen(upload)");
            }
            upload.started = true;
            starting = true;
            FileStreamHeader header;
            header.transfer_id = transfer_id;
            header.offset = upload.next_offset;
            header_bytes.resize(kFileStreamHeaderSize);
            encode_file_stream_header(header, header_bytes.data());
        }
        stream = upload.stream;
    }
    Error sent = starting
                     ? impl.send_bytes(
                           stream, std::move(header_bytes),
                           static_cast<QUIC_SEND_FLAGS>(QUIC_SEND_FLAG_START |
                                                        QUIC_SEND_FLAG_FIN))
                     : impl.send_stream_fin(stream);
    if (sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.outgoing_uploads.find(transfer_id);
        if (it != impl.outgoing_uploads.end()) {
            it->second.finished = true;
        }
    }
    return sent;
}

void QuicSession::cancel_file_upload(uint32_t transfer_id) {
    Impl& impl = *impl_;
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.outgoing_uploads.find(transfer_id);
        if (it == impl.outgoing_uploads.end()) {
            return;
        }
        stream = it->second.stream;
        it->second.stream = nullptr;
        impl.outgoing_uploads.erase(it);
    }
    if (stream) {
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        impl.api->StreamClose(stream);
    }
}

Error QuicSession::send_file_cancel(uint32_t transfer_id, FileCode code,
                                    const std::string& reason) {
    FileCancel cancel;
    cancel.transfer_id = transfer_id;
    cancel.code = code;
    cancel.reason = reason;
    // A cancellation is advisory and tiny: never let it stall the frame loop.
    if (!impl_->try_send_bytes(impl_->control.load(), make_file_cancel(cancel),
                               QUIC_SEND_FLAG_NONE)) {
        return Error(Status::Unavailable, "the control stream is closed");
    }
    return Error::success();
}

bool QuicSession::take_file_result(FileResult& result) {
    Impl& impl = *impl_;
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.file_results.empty()) {
            return false;
        }
        result = std::move(impl.file_results.front());
        impl.file_results.pop_front();
        const auto it = impl.outgoing_uploads.find(result.transfer_id);
        if (it != impl.outgoing_uploads.end()) {
            stream = it->second.stream;
            it->second.stream = nullptr;
            impl.outgoing_uploads.erase(it);
        }
    }
    if (stream) {
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        impl.api->StreamClose(stream);
    }
    return true;
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
