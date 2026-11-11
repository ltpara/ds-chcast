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
#include <zstd.h>

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
#include "chast/user_context.h"

namespace chast {

namespace {

constexpr size_t kMaxInFlightBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxInFlightSends = 4096;
constexpr size_t kMaxPendingVideoFrames = 60;
constexpr size_t kMaxDisplaySubscriptionRequests = 32;
constexpr size_t kMaxPendingVideoBytes = 8u * 1024u * 1024u;
constexpr uint64_t kVideoDeliveryWindowUs = 500000;
constexpr uint64_t kKeepAliveMs = 5000;

// v35 datagram audio. A 20 ms stereo packet at the configured 128 kbps is
// about 370 bytes; requiring 512 bytes of payload budget keeps datagram mode
// off a path whose first datagram would not even carry one frame.
constexpr size_t kMinAudioDatagramBudget = kAudioHeaderSize + 512;
// The viewer drains this queue on every media poll. The bound is generous
// (several seconds of 20 ms packets) but stops a stalled consumer from
// growing without limit.
constexpr size_t kMaxAudioDatagramQueue = 256;

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
// Media streams get the same backpressure: a consumer that stalls (a paused
// viewer, a host that stops polling the microphone) leaves further bytes with
// MsQuic instead of letting the peer grow an in-memory queue without limit.
// The stream resumes once the queue has drained to half of its high water.
constexpr size_t kVideoReceiveHighWaterBytes = 32u * 1024u * 1024u;
constexpr size_t kAudioReceiveHighWaterBytes = 4u * 1024u * 1024u;
constexpr size_t kMicrophoneReceiveHighWaterBytes = 4u * 1024u * 1024u;
constexpr size_t kCameraReceiveHighWaterBytes = 16u * 1024u * 1024u;
// Voice call audio rides the control stream. The queue holds about five
// seconds; past that the oldest packet is dropped, because late voice is
// worse than a short gap.
constexpr size_t kMaxVoiceAudioPackets = 256;
constexpr size_t kMaxVoiceAudioBytes = 1024u * 1024u;
constexpr size_t kMaxQueuedOffers = 8;
// One forwarded connection never buffers more than twice the protocol's high
// water mark; above the high-water mark the receive stream is paused, so the
// hard bound only catches a peer that ignores flow control.
constexpr size_t kMaxBufferedPortForwardBytes = kPortForwardReceiveHighWaterBytes * 2;
constexpr size_t kMaxQueuedPortForwardStops = 16;

// Windows defines max() as a macro, so the qualifier is wrapped before the
// call. This marks "wait as long as it takes" for the shared send queue.
const std::chrono::steady_clock::time_point kNoSendDeadline =
    (std::chrono::steady_clock::time_point::max)();

// A producer must not wait forever on completions that a half-dead
// connection may never deliver. Ten seconds is far beyond a healthy drain,
// yet short enough for the caller to surface the stuck transport as an error.
constexpr std::chrono::milliseconds kSendCapacityTimeout{10000};

// One context per thread and direction. The sender compresses on whichever
// application thread pushes chunks; the receiver decompresses on MsQuic's
// callback thread. Contexts are reused across blocks so a multi-gigabyte
// transfer does not allocate one per 64 KiB.
struct ZstdContexts {
    ZSTD_CCtx* compressor = nullptr;
    ZSTD_DCtx* decompressor = nullptr;
    ZstdContexts()
        : compressor(ZSTD_createCCtx()), decompressor(ZSTD_createDCtx()) {}
    ~ZstdContexts() {
        if (compressor) ZSTD_freeCCtx(compressor);
        if (decompressor) ZSTD_freeDCtx(decompressor);
    }
    ZstdContexts(const ZstdContexts&) = delete;
    ZstdContexts& operator=(const ZstdContexts&) = delete;
};

ZstdContexts& zstd_contexts() {
    static thread_local ZstdContexts contexts;
    return contexts;
}

// Appends one v2 block for `data`. The caller writes a raw block instead when
// this returns false, so the wire never carries a block that grew in flight.
bool append_compressed_file_block(const uint8_t* data, size_t size,
                                  std::vector<uint8_t>& out) {
    ZSTD_CCtx* context = zstd_contexts().compressor;
    if (!context || size == 0 || size > kFileBlockMaxRawBytes) {
        return false;
    }
    std::vector<uint8_t> compressed(ZSTD_compressBound(size));
    const size_t produced =
        ZSTD_compressCCtx(context, compressed.data(), compressed.size(), data, size, 1);
    if (ZSTD_isError(produced) || produced >= size ||
        produced > kFileBlockMaxStoredBytes) {
        return false;
    }
    FileBlockHeader header;
    header.stored_size = static_cast<uint32_t>(produced);
    header.raw_size = static_cast<uint32_t>(size);
    header.compressed = true;
    uint8_t block_header[kFileBlockHeaderSize];
    encode_file_block_header(header, block_header);
    out.insert(out.end(), block_header, block_header + kFileBlockHeaderSize);
    out.insert(out.end(), compressed.begin(), compressed.begin() + produced);
    return true;
}

void append_raw_file_block(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    FileBlockHeader header;
    header.stored_size = static_cast<uint32_t>(size);
    header.raw_size = static_cast<uint32_t>(size);
    header.compressed = false;
    uint8_t block_header[kFileBlockHeaderSize];
    encode_file_block_header(header, block_header);
    out.insert(out.end(), block_header, block_header + kFileBlockHeaderSize);
    out.insert(out.end(), data, data + size);
}

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

PermissionMask permission_mask_from_info(const ServerInfo& info) {
    PermissionMask mask = 0;
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
    if (info.privacy_mode_enabled) {
        mask |= kPermissionPrivacyMode;
    }
    if (info.port_forward_enabled) {
        mask |= kPermissionPortForward;
    }
    if (info.terminal_enabled) {
        mask |= kPermissionTerminal;
    }
    if (info.camera_enabled) {
        mask |= kPermissionCamera;
    }
    if (info.record_enabled) {
        mask |= kPermissionRecord;
    }
    if (info.restart_enabled) {
        mask |= kPermissionRestart;
    }
    if (info.block_input_enabled) {
        mask |= kPermissionBlockInput;
    }
    if (info.os_login_enabled) {
        mask |= kPermissionOsLogin;
    }
    if (info.session_select_enabled) {
        mask |= kPermissionSessionSelect;
    }
    if (info.remote_config_enabled) {
        mask |= kPermissionRemoteConfig;
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
PCCERT_CONTEXT find_stored_identity(HCERTSTORE store, const std::string& expected) {
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
        if (!expected.empty() && certificate_fingerprint(current) != expected) continue;
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
PCCERT_CONTEXT open_or_create_identity(IdentityScope scope, const std::string& expected, std::string* error) {
    const DWORD store_location = scope == IdentityScope::LocalMachine
                                     ? CERT_SYSTEM_STORE_LOCAL_MACHINE
                                     : CERT_SYSTEM_STORE_CURRENT_USER;
    // Open the existing store first: a create-capable open asks CryptoAPI for
    // write access even when the store exists, which some hardened profiles
    // deny. Only fall back to letting CryptoAPI create a genuinely absent one.
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     store_location | CERT_STORE_OPEN_EXISTING_FLAG, L"MY");
    if (!store) {
        store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, store_location, L"MY");
    }
    if (!store) {
        *error = std::string("cannot open the ") +
                 (scope == IdentityScope::LocalMachine ? "machine" : "current user's") +
                 " certificate store, GetLastError=" + std::to_string(GetLastError());
        return nullptr;
    }
    PCCERT_CONTEXT existing = find_stored_identity(store, expected);
    if (existing) {
        CertCloseStore(store, 0);
        return existing;
    }
    if (!expected.empty()) {
        CertCloseStore(store, 0);
        *error = "the original host certificate is unavailable; refusing to replace its identity";
        return nullptr;
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
        // The send generation this request was admitted under. A completion
        // that arrives after the queue was reset for a new connection carries
        // the old value and must not touch the new counters.
        uint64_t generation = 0;
        bool screenshot = false;
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
    // False for a session accepted by a multi-client listener: its api table
    // is its own, while the registration, configuration, certificate and
    // listener belong to the listener and must outlive the connection.
    bool owns_transport = true;
    // Multi-client listener state. `accepted` holds connections that finished
    // the TLS handshake and wait for the application; `live_clients` counts
    // every accepted session, including one the application already holds, so
    // the cap is enforced before another handshake begins.
    std::shared_ptr<std::atomic<size_t>> live_clients;
    std::shared_ptr<std::atomic<size_t>> authenticated_clients;
    bool authenticated_slot = false;
    std::mutex accept_mutex;
    std::condition_variable accept_cv;
    std::deque<std::unique_ptr<QuicSession>> accepted_sessions;
    bool listener_closed = false;
    std::atomic<HQUIC> connection{nullptr};
    std::atomic<HQUIC> control{nullptr};
    std::atomic<HQUIC> video{nullptr};
    std::atomic<HQUIC> audio{nullptr};
    // v35 audio over QUIC DATAGRAM. DATAGRAM_STATE_CHANGED reports whether
    // the peer accepts datagrams and the largest one its path can carry.
    std::atomic<bool> datagram_send_enabled{false};
    std::atomic<uint16_t> datagram_max_send_length{0};
    // Server: the viewer's Hello capability. Client: unused. The latched
    // mode is fixed at announce() so both sides agree for the whole session.
    std::atomic<bool> client_audio_datagram{false};
    std::atomic<bool> audio_datagram_active{false};
    // Server: the accepted viewer-initiated CMIC stream. Client: the stream
    // this side opened for the microphone. Null while the microphone is off.
    std::atomic<HQUIC> microphone{nullptr};

    mutable std::mutex mutex;
    std::condition_variable cv;
    bool connected = false;
    std::chrono::steady_clock::time_point connected_at{};
    // The pre-Hello timeout only guards the handshake. Once a client was
    // admitted, a later wait_for_client() must not tear down the live session.
    bool client_accepted = false;
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
    VideoCodecMask decoding_codecs = kVideoCodecAll;
    VideoCodecMask decoding_444 = 0;
    VideoChroma initial_chroma_preference = VideoChroma::Yuv420;
    SessionMode requested_mode = SessionMode::Desktop;
    std::string requested_peer;
    // Audit reference of the accepted Hello. pending_hello is consumed by
    // wait_for_client, so the accessor reads this copy instead.
    std::string session_audit_ref;
    ClientHello pending_hello;
    // v62: the challenge the server sent before Hello, and whether one went
    // out. The application thread verifies the ticket possession proof with
    // it. On the client side the same field is the host's challenge once
    // auth_challenge_ready is set.
    std::array<uint8_t, kTicketProofHostNonceSize> host_nonce{};
    bool host_nonce_sent = false;
    bool auth_challenge_ready = false;
    // Server: the newest v62 ticket renewal the viewer sent. Client: the
    // newest answer to this side's renewal, and the Hello nonce the accepted
    // handshake used (a renewal proof covers it).
    bool ticket_renew_ready = false;
    TicketRenewRequest ticket_renew;
    bool ticket_renew_result_ready = false;
    TicketRenewResult ticket_renew_result;
    uint64_t ticket_renew_results_seen = 0;
    std::array<uint8_t, kHelloNonceBytes> client_nonce{};
    StreamCodecPreference initial_codec_preference = StreamCodecPreference::Auto;
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
    std::deque<CaptureDisplaysRequest> capture_display_requests;
    std::deque<CaptureDisplaysResult> capture_display_results;
    std::map<uint32_t, uint16_t> subscribed_display_streams;
    std::map<uint32_t, DisplayStreamInfo> display_stream_updates;
    std::map<uint32_t, CursorUpdate> display_cursor_updates;
    std::deque<uint32_t> display_keyframe_requests;
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
    // Server: the newest SetDisplayFollow request. Client: the newest state
    // the host reported, also used for a host-initiated disable.
    bool display_follow_request_ready = false;
    uint8_t display_follow_flags = 0;
    bool display_follow_state_ready = false;
    DisplayFollowState display_follow_state;
    uint64_t display_follow_states_seen = 0;
    // Server: the newest show-cursor hint. The overlay only needs the latest
    // position, so older hints are replaced rather than queued.
    bool cursor_hint_ready = false;
    ShowCursorHint cursor_hint;
    uint64_t cursor_hints_seen = 0;
    // Server: the newest SetPrivacyMode request. Client: the newest state the
    // host sent, also used for a host-initiated disable.
    bool privacy_mode_request_ready = false;
    bool privacy_mode_requested = false;
    uint64_t privacy_mode_requests_seen = 0;
    bool privacy_mode_state_ready = false;
    bool privacy_mode_enabled = false;
    PrivacyModeCode privacy_mode_code = PrivacyModeCode::Ok;
    std::string privacy_mode_reason;
    uint64_t privacy_mode_states_seen = 0;
    // Client: the newest desktop kind the host announced.
    bool desktop_state_ready = false;
    DesktopKind desktop_kind = DesktopKind::Default;
    uint64_t desktop_states_seen = 0;
    // Server: SAS requests waiting for the host loop, and the total seen.
    // Client: the newest answer, with its own counter.
    size_t sas_requests_pending = 0;
    uint64_t sas_requests_seen = 0;
    bool sas_result_ready = false;
    SasCode sas_code = SasCode::Ok;
    std::string sas_reason;
    uint64_t sas_results_seen = 0;
    // Host-control requests and answers are short ordered queues: a toggle
    // sequence must run in order, and every action produces one answer.
    std::deque<HostControlRequest> host_action_requests;
    std::deque<uint32_t> elevation_requests;
    std::deque<ElevationState> elevation_states;
    uint64_t host_action_requests_seen = 0;
    std::deque<HostControlResult> host_action_results;
    uint64_t host_action_results_seen = 0;
    // Server: the newest RemoteConfigRequest. Client: the newest state the
    // host answered, coalesced like the other one-shot control replies.
    bool remote_config_request_ready = false;
    RemoteConfigRequest remote_config_request;
    uint64_t remote_config_requests_seen = 0;
    bool remote_config_state_ready = false;
    RemoteConfigState remote_config_state;
    uint64_t remote_config_states_seen = 0;
    // Server: the newest DisplayControl request. Client: the newest state the
    // host answered, coalesced like the other one-shot control replies.
    bool display_control_request_ready = false;
    DisplayControlRequest display_control_request;
    uint64_t display_control_requests_seen = 0;
    bool display_control_state_ready = false;
    DisplayControlState display_control_state;
    uint64_t display_control_states_seen = 0;
    // Server: OS-login passwords waiting for the host loop. Short and bounded
    // like host actions; dropped, overflowed and reset entries are wiped.
    std::deque<std::string> os_login_requests;
    uint64_t os_login_requests_seen = 0;
    // Client: the newest OS-login answer, which carries the reason.
    bool os_login_result_ready = false;
    OsLoginCode os_login_code = OsLoginCode::Ok;
    std::string os_login_reason;
    uint64_t os_login_results_seen = 0;
    // Server: a session list request is pending, and the newest select request
    // waiting for the host loop. A list is coalesced because only the current
    // session set matters; a select replaces an older one because the viewer
    // can only be heading to one session.
    bool session_list_request_pending = false;
    uint64_t session_list_requests_seen = 0;
    bool session_select_request_ready = false;
    uint32_t session_select_request_id = 0;
    uint64_t session_select_requests_seen = 0;
    // Client: the newest session list and select answers.
    bool session_list_result_ready = false;
    SessionListResult session_list_result;
    uint64_t session_list_results_seen = 0;
    bool session_select_result_ready = false;
    SessionSelectResult session_select_result;
    uint64_t session_select_results_seen = 0;
    // Server: the newest SetStreamOptions request. Client: the newest answer,
    // carrying the effective settings even when the request was refused.
    bool stream_options_request_ready = false;
    StreamOptions stream_options_request;
    uint64_t stream_options_requests_seen = 0;
    bool stream_options_result_ready = false;
    StreamOptionsResult stream_options_result;
    uint64_t stream_options_results_seen = 0;
    // Server: the mask the viewer was granted, and the ceiling that grant
    // established. Mid-session changes may only stay inside the ceiling.
    // Client: the mask the host last announced. Atomic because the transport
    // callbacks read it while the application thread updates it.
    std::atomic<PermissionMask> session_permissions{0};
    std::atomic<ClipboardDirection> peer_clipboard_direction{ClipboardDirection::Both};
    PermissionMask pending_policy_revocations = 0;  // guarded by mutex
    PermissionMask permission_ceiling = 0;
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
    // A media byte queue with its own byte count. All fields are guarded by
    // `mutex`: the receive callback fills it, the application drains it, and
    // `paused` records that the receive stream was disabled at the high-water
    // mark. The consumer re-enables the stream while still holding the lock,
    // so it cannot use a handle that SHUTDOWN_COMPLETE already closed.
    struct MediaQueue {
        std::deque<std::vector<uint8_t>> chunks;
        size_t bytes = 0;
        bool paused = false;

        void push(std::vector<uint8_t> chunk) {
            bytes += chunk.size();
            chunks.push_back(std::move(chunk));
        }
        void emplace(const uint8_t* begin, const uint8_t* end) {
            bytes += static_cast<size_t>(end - begin);
            chunks.emplace_back(begin, end);
        }
        bool empty() const { return chunks.empty(); }
        std::vector<uint8_t> take_front() {
            std::vector<uint8_t> chunk = std::move(chunks.front());
            chunks.pop_front();
            bytes -= chunk.size();
            return chunk;
        }
        void clear() {
            chunks.clear();
            bytes = 0;
            paused = false;
        }
    };

    MediaQueue received;
    MediaQueue audio_received;
    // Client: whole audio packets that arrived as QUIC DATAGRAM frames. They
    // are served before the stream parser so a reordered datagram can never
    // be spliced into a partially received stream packet.
    std::deque<std::vector<uint8_t>> audio_datagrams;
    // Client: first time receive_audio noticed the audio stream FIN while
    // datagram audio was active. Datagrams and the FIN travel separate paths,
    // so the last packets may still be in flight; the reader waits out a short
    // quiet period instead of reporting a loss that never happened.
    std::chrono::steady_clock::time_point audio_datagram_drain_until{};
    // Server: datagram payloads are owned by the application until MsQuic
    // reports a final send state. The callback frees the context; this list
    // also releases leftovers during teardown.
    struct DatagramSend {
        std::vector<uint8_t> bytes;
        QUIC_BUFFER buffer{};
    };
    std::mutex datagram_mutex;
    std::vector<DatagramSend*> datagram_sends;
    ControlParser control_parser;
    FrameParser frame_parser;
    AudioParser audio_parser;
    // Microphone uplink (CMIC). `microphone_handles` keeps every CMIC handle
    // this side accepted, including one that was replaced by a newer stream,
    // so late events close the old handle instead of being classified again.
    MediaQueue microphone_received;
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
        bool clipboard = false;
        HQUIC stream = nullptr;
        uint64_t start_offset = 0;
        uint64_t received = 0;
        uint64_t wire_bytes = 0;
        bool compressed = false;
        bool finished = false;
        bool receive_paused = false;
        // v2 wire bytes that do not yet form a whole block. Bounded by one
        // block header plus one maximum stored block, because the loop drains
        // every complete block as soon as it arrives.
        std::vector<uint8_t> block_pending;
        std::string error;
    };
    struct OutgoingUpload {
        FileOffer offer;
        bool accepted = false;
        bool started = false;
        bool finished = false;
        bool compress = false;  // v2 block stream, chosen when the stream opens
        HQUIC stream = nullptr;
        uint64_t next_offset = 0;
    };
    // One forwarded TCP connection (protocol v26). The viewer owns the uplink
    // and the host owns the downlink, so each side has exactly one send and
    // one receive stream; the ids correlate them across the two roles.
    struct PortForwardTarget {
        std::string host;
        uint16_t port = 0;
    };
    struct PortForwardEntry {
        PortForwardHandle handle = 0;
        uint32_t forward_id = 0;
        uint32_t connection_id = 0;
        bool server_side = false;
        HQUIC uplink = nullptr;    // viewer -> host; open on both sides
        HQUIC downlink = nullptr;  // host -> viewer; open after the dial
        // Server: the host accepted the dial. Client: the downlink arrived,
        // so the host's dial succeeded.
        bool accepted = false;
        bool send_finished = false;
        bool receive_finished = false;
        bool failed = false;
        // The application closed the handle after both directions finished
        // cleanly. The streams stay open until QUIC reports SHUTDOWN_COMPLETE,
        // because aborting them here would discard bytes the peer has not
        // acknowledged yet.
        bool app_closed = false;
        std::string error;
        std::deque<std::vector<uint8_t>> received;
        size_t buffered_bytes = 0;
        bool receive_paused = false;
    };
    std::vector<ClipboardStream> clipboard_streams;
    size_t pending_clipboard_bytes = 0;
    std::deque<ClipboardContent> clipboard_contents;
    std::deque<ClipboardFileRead> clipboard_file_reads;
    std::deque<ClipboardFileData> clipboard_file_data;
    std::map<uint32_t, ClipboardFileRead> requested_clipboard_reads;
    std::map<uint32_t, ClipboardFileRead> accepted_clipboard_reads;
    uint64_t clipboard_contents_seen = 0;
    // Chat lines waiting for the application. Chat is low-rate; a full queue
    // drops the newest message rather than replaying stale conversation.
    std::deque<std::string> chats;
    uint64_t chats_seen = 0;
    // Composed text blocks waiting for the application. Like input events,
    // this queue is for observation; the injector consumes them itself.
    std::deque<std::string> texts;
    uint64_t texts_seen = 0;
    // Voice call (protocol v37). The host microphone packets ride the control
    // stream; the queues stay small because stale voice is useless. The
    // server keeps at most one pending start, the client one outstanding
    // request id.
    bool server_voice_active = false;
    bool client_voice_active = false;
    bool client_voice_requested = false;
    uint64_t client_voice_request_id = 0;
    uint64_t server_voice_pending = 0;
    uint64_t next_voice_request_id = 0;
    uint64_t next_voice_seq = 0;
    bool voice_closed_notice = false;
    uint64_t voice_audio_seen = 0;
    std::deque<VoiceCallRequest> voice_requests;
    std::deque<VoiceCallResponse> voice_responses;
    std::deque<std::vector<uint8_t>> voice_audio;
    size_t buffered_voice_bytes = 0;
    // Host screenshot (protocol v70). The host queues a bounded number of
    // requests; the client assembles one answer at a time in a buffer that the
    // message parser and the chunk layout both bound.
    std::deque<ScreenshotRequest> screenshot_requests;
    struct ScreenshotStream { HQUIC stream; ControlParser parser; };
    std::vector<ScreenshotStream> screenshot_streams;
    HQUIC outgoing_screenshot = nullptr;
    uint64_t outgoing_screenshot_request = 0;
    uint32_t outgoing_screenshot_next = 0;
    uint64_t next_screenshot_request_id = 0;
    uint64_t client_screenshot_request_id = 0;
    uint32_t screenshot_total = 0;
    uint32_t screenshot_count = 0;
    uint32_t screenshot_next = 0;
    uint32_t screenshot_stream = 0;
    uint32_t screenshot_width = 0;
    uint32_t screenshot_height = 0;
    std::vector<uint8_t> screenshot_bytes;
    std::deque<ScreenshotResult> screenshots;
    std::deque<FileOffer> file_offers;
    uint64_t file_offers_seen = 0;
    std::deque<FileChunk> file_chunks;
    size_t buffered_file_bytes = 0;
    std::map<uint32_t, ReceivedUpload> received_uploads;
    struct IncomingFileOffer { bool clipboard = false; bool accepted = false; };
    std::map<uint32_t, IncomingFileOffer> incoming_file_offers;
    std::deque<FileCancel> file_cancels;
    std::deque<FileConflict> file_conflicts;
    std::deque<FileConflictAnswer> file_conflict_answers;
    std::map<uint32_t, OutgoingUpload> outgoing_uploads;
    std::deque<FileAnswer> file_answers;
    std::deque<FileResult> file_results;
    // Shared-folder browsing (protocol v30). Requests keep the viewer's id so
    // every one is answered; results keep the host's id for the same reason.
    std::deque<FileListRequest> file_list_requests;
    std::deque<FileListResult> file_list_results;
    std::deque<std::pair<uint32_t, std::string>> file_download_requests;
    std::deque<FileDownloadResult> file_download_results;
    // File management (protocol v31). The request keeps the viewer's id and
    // every answer keeps it too, so each is matched like a browse request.
    struct FileOpRequestData {
        uint32_t request_id = 0;
        FileOperation op = FileOperation::MakeDirectory;
        std::string path;
        std::string new_name;
    };
    std::deque<FileOpRequestData> file_op_requests;
    std::deque<FileOpResult> file_op_results;
    // Port forwarding. Targets are what the viewer registered and the host
    // approved; entries are the live TCP connections. `pending_*` keeps the
    // request between take_port_forward_request and the host's answer.
    std::map<uint32_t, PortForwardTarget> active_forward_targets;
    std::map<uint32_t, PortForwardTarget> client_forward_targets;
    std::map<uint32_t, PortForwardTarget> client_pending_forwards;
    std::map<uint32_t, PortForwardTarget> pending_forward_targets;
    std::map<PortForwardHandle, PortForwardEntry> port_forward_connections;
    std::deque<PortForwardRequest> port_forward_requests;
    std::deque<PortForwardResult> port_forward_results;
    std::deque<PortForwardOpen> port_forward_opens;
    std::deque<PortForwardStop> port_forward_stops;
    uint64_t next_port_forward_handle = 1;
    // Remote terminals (protocol v27). Both roles create the entry when the
    // host answers Ok, so the two CTTY streams can attach in either order:
    // ConPTY may print its prompt before the viewer opens its input stream.
    struct TerminalEntry {
        uint32_t terminal_id = 0;
        bool server_side = false;
        uint16_t columns = 0;
        uint16_t rows = 0;
        HQUIC uplink = nullptr;    // viewer -> host input
        HQUIC downlink = nullptr;  // host -> viewer output
        bool send_finished = false;
        bool receive_finished = false;
        // The peer sent TerminalStop. The viewer keeps the entry so console
        // output already on the wire can still be drained; the host closes its
        // console and forgets the terminal at once.
        bool stopped = false;
        bool failed = false;
        std::string error;
        // Console output on the viewer, keyboard input on the host.
        std::deque<std::vector<uint8_t>> received;
        size_t buffered_bytes = 0;
        bool receive_paused = false;
    };
    std::map<uint32_t, TerminalSize> pending_terminal_targets;
    std::map<uint32_t, TerminalSize> active_terminal_targets;
    std::map<uint32_t, TerminalSize> client_pending_terminals;
    std::map<uint32_t, TerminalSize> client_terminal_targets;
    std::map<uint32_t, TerminalEntry> terminal_entries;
    std::deque<TerminalRequest> terminal_requests;
    std::deque<TerminalResult> terminal_results;
    std::deque<TerminalStop> terminal_stops;
    std::deque<TerminalSize> terminal_resizes;
    // Camera (protocol v29). One CCAM stream at a time, opened by the host
    // after a successful StartCamera. The viewer keeps the parsed frames in
    // `camera_received`/`camera_parser`; the host only sends. The pending and
    // running ids both name a stream the viewer will accept, because the CCAM
    // stream and the CameraResult travel on different streams and may cross.
    std::atomic<HQUIC> camera{nullptr};
    std::atomic<uint64_t> next_camera_seq{0};
    bool camera_finished = false;
    uint16_t server_camera_id = 0;
    // Host: the start the host application has not answered yet. It keeps the
    // one-at-a-time rule intact while the device is still opening.
    uint16_t server_pending_camera = 0;
    uint16_t client_pending_camera = 0;
    uint16_t client_camera_id = 0;
    bool camera_list_request_ready = false;
    std::deque<CameraList> camera_lists_queue;
    uint64_t camera_lists_seen = 0;
    std::deque<uint16_t> camera_starts;
    std::deque<CameraResult> camera_results;
    std::deque<uint16_t> camera_stops;
    MediaQueue camera_received;
    FrameParser camera_parser;
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
    uint64_t pending_qos_probe_us = 0;
    uint64_t last_qos_probe_send_us = 0;

    struct VideoSend { uint64_t sequence, sent_us, capture_us; size_t bytes; };
    std::deque<VideoSend> pending_video;
    size_t pending_video_bytes = 0;
    std::optional<uint64_t> pending_video_receipt;
    // Serialize sequence assignment with StreamSend; a refused send must not
    // leave a sequence gap, nor can concurrent callers reorder the stream.
    std::mutex video_send_mutex;

    std::atomic<uint64_t> next_seq{0};
    std::atomic<uint64_t> next_audio_seq{0};
    std::atomic<uint64_t> next_microphone_seq{0};
    std::atomic<bool> transport_down{false};

    std::mutex send_mutex;
    std::condition_variable send_cv;
    size_t in_flight_bytes = 0;
    size_t screenshot_in_flight_bytes = 0;
    size_t in_flight_sends = 0;
    uint64_t send_generation = 0;

    ~Impl() {
        close();
        if (authenticated_slot) authenticated_clients->fetch_sub(1);
        if (live_clients) {
            live_clients->fetch_sub(1);
        }
    }

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

    // A malformed control message always ends its connection, but before the
    // handshake completed it must not also end the listener: anyone who
    // finishes TLS can send garbage, and letting that tear down the host's
    // single session slot turns it into a remotely triggerable outage. Only a
    // violation by an established session escalates to transport_failed.
    void control_protocol_failure(const std::string& message) {
        bool pre_auth = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            // A client has no listener to protect, so its handshake keeps the
            // descriptive protocol error; only the server's pre-approval
            // window is downgraded to "drop this connection".
            pre_auth = role == Role::Server && !client_accepted;
        }
        if (!pre_auth) {
            note_transport_failure(message);
            return;
        }
        if (HQUIC handle = connection.load()) {
            api->ConnectionShutdown(handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
        cv.notify_all();
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

    bool desktop_subscription_ready() const {
        std::lock_guard<std::mutex> lock(mutex);
        // Requests may reach the server immediately after HelloAck, before
        // announce() returns. Likewise initial metadata can reach the client
        // before handshake() wakes. Gate on accepted/parsed authentication,
        // not whether the application's blocking call has returned yet.
        return role == Role::Server
            ? client_accepted && requested_mode == SessionMode::Desktop
            : hello_ack_ready && client_config.session_mode == SessionMode::Desktop;
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
        HQUIC incoming = event.NEW_CONNECTION.Connection;
        if (server_config.max_clients > 1) {
            return on_new_viewer(incoming);
        }
        HQUIC expected = nullptr;
        if (!connection.compare_exchange_strong(expected, incoming)) {
            // The tool serves one viewer at a time; a second connection is
            // refused before the handshake continues. Shutting it down first
            // tells that client right away instead of leaving it retransmit.
            api->ConnectionShutdown(incoming, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(incoming);
            return QUIC_STATUS_SUCCESS;
        }
        api->SetCallbackHandler(incoming,
                                reinterpret_cast<void*>(&Impl::connection_callback), this);
        const QUIC_STATUS status = api->ConnectionSetConfiguration(incoming, configuration);
        if (QUIC_FAILED(status)) {
            fail(Status::Unavailable,
                 "ConnectionSetConfiguration failed: " + status_hex(status));
            api->ConnectionClose(incoming);
            connection = nullptr;
            return status;
        }
        return QUIC_STATUS_SUCCESS;
    }

    // Multi-client listener: builds a server session for the connection the
    // listener just accepted. Runs on the MsQuic listener callback, so it only
    // allocates and registers; the Hello, approval and media all happen on the
    // application thread that calls accept().
    QUIC_STATUS on_new_viewer(HQUIC incoming) {
        {
            std::lock_guard<std::mutex> lock(accept_mutex);
            if (listener_closed) {
                api->ConnectionShutdown(incoming, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                api->ConnectionClose(incoming);
                return QUIC_STATUS_SUCCESS;
            }
        }
        const size_t live = live_clients->fetch_add(1) + 1;
        if (live > server_config.max_clients) {
            live_clients->fetch_sub(1);
            api->ConnectionShutdown(incoming, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(incoming);
            return QUIC_STATUS_SUCCESS;
        }
        const auto decline = [&]() {
            live_clients->fetch_sub(1);
            api->ConnectionShutdown(incoming, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(incoming);
            return QUIC_STATUS_SUCCESS;
        };

        std::unique_ptr<QuicSession> child(new QuicSession());
        child->impl_ = std::make_unique<Impl>();
        Impl& target = *child->impl_;
        target.role = Role::Server;
        target.server_config = server_config;
        // One connection per accepted session: the helper never listens and
        // never waits for a replacement viewer.
        target.server_config.max_clients = 1;
        target.server_config.stop_after_denial = true;
        target.server_config.access_token = server_config.access_token;
        target.api = nullptr;
        if (QUIC_FAILED(MsQuicOpen2(&target.api))) {
            return decline();
        }
        target.owns_transport = false;
        target.live_clients = live_clients;
        target.authenticated_clients = authenticated_clients;
        target.connection = incoming;
        api->SetCallbackHandler(incoming,
                                reinterpret_cast<void*>(&Impl::connection_callback), &target);
        const QUIC_STATUS status = api->ConnectionSetConfiguration(incoming, configuration);
        if (QUIC_FAILED(status)) {
            target.connection = nullptr;
            target.live_clients.reset();
            return decline();
        }
        {
            std::lock_guard<std::mutex> lock(accept_mutex);
            if (listener_closed) {
                target.connection = nullptr;
                target.live_clients.reset();
                return decline();
            }
            accepted_sessions.push_back(std::move(child));
        }
        accept_cv.notify_all();
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS on_connection(QUIC_CONNECTION_EVENT& event) {
        switch (event.Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    connected = true;
                    connected_at = std::chrono::steady_clock::now();
                }
                cv.notify_all();
                break;
            }
            case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED: {
                datagram_send_enabled.store(event.DATAGRAM_STATE_CHANGED.SendEnabled != FALSE);
                datagram_max_send_length.store(event.DATAGRAM_STATE_CHANGED.MaxSendLength);
                break;
            }
            case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
                if (role != Role::Client) {
                    break;
                }
                const QUIC_BUFFER* buffer = event.DATAGRAM_RECEIVED.Buffer;
                if (!buffer || buffer->Length < kAudioHeaderSize ||
                    buffer->Length > kAudioHeaderSize + kMaxAudioPayload) {
                    break;
                }
                // A malformed datagram is dropped in place: there is no byte
                // stream to resynchronise, and one bad packet must not end a
                // session the reliable streams keep alive.
                AudioHeader header;
                std::string error;
                if (!decode_audio_header(buffer->Buffer, buffer->Length, header, &error) ||
                    header.payload_size == 0 ||
                    header.payload_size != buffer->Length - kAudioHeaderSize) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (audio_datagrams.size() >= kMaxAudioDatagramQueue) {
                        audio_datagrams.pop_front();
                    }
                    audio_datagrams.emplace_back(buffer->Buffer,
                                                 buffer->Buffer + buffer->Length);
                }
                cv.notify_all();
                break;
            }
            case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
                void* context = event.DATAGRAM_SEND_STATE_CHANGED.ClientContext;
                if (!context ||
                    !QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
                        event.DATAGRAM_SEND_STATE_CHANGED.State)) {
                    break;
                }
                auto* send = static_cast<DatagramSend*>(context);
                {
                    std::lock_guard<std::mutex> lock(datagram_mutex);
                    datagram_sends.erase(
                        std::remove(datagram_sends.begin(), datagram_sends.end(), send),
                        datagram_sends.end());
                }
                delete send;
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
                    constexpr uint32_t kViewerStreamPermissions =
                        kPermissionClipboard | kPermissionFileTransfer |
                        kPermissionMicrophone | kPermissionPortForward | kPermissionTerminal;
                    if (role == Role::Server &&
                        (effective_permissions() & kViewerStreamPermissions) == 0) {
                        // None of the viewer-initiated stream types (clipboard,
                        // file, microphone, port forward, terminal) is granted
                        // for this session; the HelloAck said so already.
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

    static void copy_chunks(MediaQueue& queue, const QUIC_STREAM_EVENT& event) {
        for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
            queue.emplace(buffer.Buffer, buffer.Buffer + buffer.Length);
        }
    }

    // Caller holds `mutex`. Copies a receive event into a media queue unless
    // the queue is already at its high-water mark. In that case the event is
    // left with MsQuic (TotalBufferLength = 0 withholds flow-control credit).
    // Returns true when the caller must disable the stream; it must do so
    // before releasing the lock so a consumer cannot resume the stream in
    // between and then have this disable leave it stalled.
    static bool queue_media_locked(MediaQueue& queue, QUIC_STREAM_EVENT& event,
                                   size_t high_water) {
        if (queue.bytes >= high_water) {
            event.RECEIVE.TotalBufferLength = 0;
            queue.paused = true;
            return true;
        }
        copy_chunks(queue, event);
        if (queue.bytes >= high_water) {
            queue.paused = true;
            return true;
        }
        return false;
    }

    // Caller holds `mutex`. Re-enables a paused media stream once its queue
    // has drained to the low-water mark. `stream` is read under the same lock
    // the SHUTDOWN_COMPLETE callback uses to forget the handle, so a closed
    // handle is never passed back to MsQuic.
    void resume_media_locked(HQUIC stream, MediaQueue& queue, size_t high_water) {
        if (stream && queue.paused && queue.bytes <= high_water / 2) {
            queue.paused = false;
            api->StreamReceiveSetEnabled(stream, TRUE);
        }
    }

    // Caller holds `mutex`. Aborts handles that were just removed from the
    // tracking tables while the same lock is held, so a concurrent
    // SHUTDOWN_COMPLETE cannot close one between the lookup and this call.
    void abort_streams_locked(const std::vector<HQUIC>& handles,
                              HQUIC skip = nullptr) {
        for (HQUIC handle : handles) {
            if (handle && handle != skip) {
                api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            }
        }
    }

    // Buffers a stream until its packet magic identifies it: video/audio on
    // the viewer, clipboard changes and file transfers on both ends, the
    // viewer's microphone on the host. Anything else is a peer that is not
    // speaking this protocol version.
    void classify_media_stream(HQUIC stream, QUIC_STREAM_EVENT& event) {
        enum class Kind {
            Wait,
            Video,
            Audio,
            Microphone,
            Clipboard,
            File,
            PortForward,
            Terminal,
            Camera,
            Screenshot,
            Reject,
            Bad
        };
        Kind kind = Kind::Wait;
        HQUIC port_forward_pause = nullptr;
        HQUIC terminal_pause = nullptr;
        std::optional<FileResult> file_rejection;
        std::vector<std::pair<ControlType, std::vector<uint8_t>>> screenshot_messages;
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
            if (std::memcmp(pending->data(), "CSHT", 4) == 0) {
                if (role != Role::Client || screenshot_streams.size() >= 2 ||
                    client_screenshot_request_id == 0) {
                    one_shot_streams.push_back(stream);
                    kind = Kind::Reject;
                } else {
                    ScreenshotStream entry{stream, {}};
                    std::string error;
                    if (!entry.parser.push(pending->data() + 4, pending->size() - 4,
                                           screenshot_messages, &error)) {
                        one_shot_streams.push_back(stream);
                        kind = Kind::Reject;
                    } else {
                        screenshot_streams.push_back(std::move(entry));
                        kind = Kind::Screenshot;
                    }
                }
            } else if (std::memcmp(pending->data(), "CFRM", 4) == 0) {
                HQUIC expected = nullptr;
                if (role == Role::Client && video.compare_exchange_strong(expected, stream)) {
                    received.push(std::move(*pending));
                    kind = Kind::Video;
                } else {
                    kind = Kind::Bad;
                }
            } else if (std::memcmp(pending->data(), "CAUD", 4) == 0) {
                HQUIC expected = nullptr;
                if (role == Role::Client && audio.compare_exchange_strong(expected, stream)) {
                    audio_received.push(std::move(*pending));
                    kind = Kind::Audio;
                } else {
                    kind = Kind::Bad;
                }
            } else if (std::memcmp(pending->data(), kMicrophoneStreamMagic, 4) == 0) {
                // Viewer to host only, and only while the grant is live. A
                // stream without the permission is refused quietly: the
                // microphone is optional and never takes video down.
                if (role != Role::Server ||
                    (effective_permissions() & kPermissionMicrophone) == 0) {
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
                                microphone_received.emplace(pending->data() + consumed,
                                                            pending->data() + pending->size());
                            }
                            ++microphone_streams_seen;
                            kind = Kind::Microphone;
                        }
                    }
                }
            } else if (std::memcmp(pending->data(), kClipboardStreamMagic, 4) == 0) {
                const bool allowed = clipboard_allowed(false);
                if (!allowed) {
                    // A revoked permission is recoverable: refuse this push,
                    // keep the session.
                    kind = Kind::Reject;
                } else if (clipboard_streams.size() >= kMaxClipboardStreams ||
                           pending_clipboard_bytes + pending->size() >
                               kMaxPendingClipboardBytes) {
                    // Quota exhaustion is recoverable too; the next item can
                    // succeed once the peer's earlier pushes finish.
                    kind = Kind::Reject;
                } else {
                    ClipboardStream entry;
                    entry.stream = stream;
                    entry.bytes = std::move(*pending);
                    pending_clipboard_bytes += entry.bytes.size();
                    clipboard_streams.push_back(std::move(entry));
                    kind = Kind::Clipboard;
                }
            } else if (std::memcmp(pending->data(), kFileStreamMagic, 4) == 0) {
                FileStreamHeader header;
                std::string error;
                const bool header_ready = pending->size() >= kFileStreamHeaderSize;
                // The first receive usually carries the header and the first
                // chunk together; decode only the fixed prefix.
                const bool header_ok =
                    header_ready &&
                    decode_file_stream_header(pending->data(), kFileStreamHeaderSize,
                                              header, &error);
                if (!header_ready) return;
                const auto offered = incoming_file_offers.find(header.transfer_id);
                const bool allowed = header_ok && offered != incoming_file_offers.end() &&
                    offered->second.accepted && file_allowed(offered->second.clipboard, false);
                if (!allowed) {
                    // Revoking file transfer must not end a live session that
                    // still has a CFIL stream in flight. When the header is
                    // already here, the upload is failed explicitly.
                    if (header_ok) {
                        file_rejection = FileResult{header.transfer_id, FileCode::Refused, 0,
                                                    "file transfer is disabled"};
                    }
                    kind = Kind::Reject;
                } else if (!header_ready) {
                    // The fixed prefix has not arrived yet.
                    return;
                } else if (!header_ok) {
                    kind = Kind::Bad;
                } else if (received_uploads.count(header.transfer_id) != 0 ||
                           received_uploads.size() >= kMaxIncomingUploads ||
                           header.offset > kMaxFileTransferBytes) {
                    // Duplicate, too many concurrent uploads, or an
                    // out-of-range resume point: refuse this stream only.
                    const bool out_of_range = header.offset > kMaxFileTransferBytes;
                    file_rejection = FileResult{
                        header.transfer_id, out_of_range ? FileCode::Refused : FileCode::Busy,
                        0,
                        out_of_range ? "the resume offset is outside the transfer limit"
                                     : "the host is not accepting another upload"};
                    kind = Kind::Reject;
                } else {
                    ReceivedUpload upload;
                    upload.clipboard = offered->second.clipboard;
                    upload.stream = stream;
                    upload.start_offset = header.offset;
                    upload.compressed = header.version == kFileStreamVersionCompressed;
                    const auto inserted =
                        received_uploads.emplace(header.transfer_id, std::move(upload));
                    if (pending->size() > kFileStreamHeaderSize) {
                        feed_file_stream_locked(
                            inserted.first->second, inserted.first->first,
                            pending->data() + kFileStreamHeaderSize,
                            pending->size() - kFileStreamHeaderSize);
                    }
                    kind = Kind::File;
                }
            } else if (std::memcmp(pending->data(), kPortForwardStreamMagic, 4) == 0) {
                // One unidirectional stream per direction per TCP connection.
                // The host only accepts an uplink for a target it approved;
                // the viewer only accepts a downlink for a connection it
                // opened. Anything else is refused quietly, like a stale
                // clipboard stream.
                if ((effective_permissions() & kPermissionPortForward) == 0) {
                    kind = Kind::Reject;
                } else if (pending->size() < kPortForwardStreamHeaderSize) {
                    return;
                } else {
                    PortForwardStreamHeader header;
                    std::string header_error;
                    if (!decode_port_forward_stream_header(pending->data(),
                                                           kPortForwardStreamHeaderSize, header,
                                                           &header_error)) {
                        kind = Kind::Bad;
                    } else if (role == Role::Server) {
                        const auto target = active_forward_targets.find(header.forward_id);
                        // The uplink pauses after its first packet, so this is
                        // the only chunk that can arrive before the host had a
                        // chance to dial.
                        const size_t excess =
                            pending->size() - kPortForwardStreamHeaderSize;
                        prune_retired_port_forwards_locked();
                        const bool duplicate =
                            has_port_forward_entry_locked(header.forward_id,
                                                          header.connection_id);
                        const size_t live = port_forward_live_count_locked();
                        if (target == active_forward_targets.end() ||
                            duplicate ||
                            live >= kMaxPortForwardConnections ||
                            excess > kMaxBufferedPortForwardBytes) {
                            kind = Kind::Reject;
                        } else {
                            PortForwardEntry entry;
                            entry.handle = next_port_forward_handle++;
                            entry.forward_id = header.forward_id;
                            entry.connection_id = header.connection_id;
                            entry.server_side = true;
                            entry.uplink = stream;
                            // The host has to dial before any byte can be
                            // relayed, so the uplink stays paused until
                            // accept_port_forward_connection() succeeds.
                            entry.receive_paused = true;
                            auto inserted = port_forward_connections.emplace(
                                entry.handle, std::move(entry));
                            PortForwardEntry& created = inserted.first->second;
                            PortForwardOpen open;
                            open.handle = created.handle;
                            open.forward_id = created.forward_id;
                            open.connection_id = created.connection_id;
                            open.host = target->second.host;
                            open.port = target->second.port;
                            port_forward_opens.push_back(std::move(open));
                            if (excess > 0) {
                                created.received.emplace_back(
                                    pending->begin() + kPortForwardStreamHeaderSize,
                                    pending->end());
                                created.buffered_bytes = excess;
                            }
                            port_forward_pause = stream;
                            kind = Kind::PortForward;
                        }
                    } else {
                        PortForwardEntry* matched = nullptr;
                        for (auto& item : port_forward_connections) {
                            PortForwardEntry& candidate = item.second;
                            if (!candidate.server_side && candidate.downlink == nullptr &&
                                !candidate.failed && candidate.forward_id == header.forward_id &&
                                candidate.connection_id == header.connection_id) {
                                matched = &candidate;
                                break;
                            }
                        }
                        if (!matched) {
                            kind = Kind::Reject;
                        } else {
                            matched->downlink = stream;
                            matched->accepted = true;
                            const size_t excess = pending->size() - kPortForwardStreamHeaderSize;
                            if (matched->buffered_bytes + excess > kMaxBufferedPortForwardBytes) {
                                std::vector<HQUIC> aborts;
                                fail_port_forward_locked(
                                    *matched, "the port forward receive queue overflowed",
                                    aborts);
                                for (HQUIC handle : aborts) {
                                    api->StreamShutdown(
                                        handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                                }
                                kind = Kind::PortForward;
                            } else {
                                if (excess > 0) {
                                    matched->received.emplace_back(
                                        pending->begin() + kPortForwardStreamHeaderSize,
                                        pending->end());
                                    matched->buffered_bytes += excess;
                                }
                                kind = Kind::PortForward;
                            }
                        }
                    }
                }
            } else if (std::memcmp(pending->data(), kTerminalStreamMagic, 4) == 0) {
                // One CTTY stream per direction per terminal. Both entries
                // exist once the host answered Ok, so either direction may
                // arrive first: ConPTY can print its prompt before the viewer
                // opens its input stream.
                if ((effective_permissions() & kPermissionTerminal) == 0) {
                    kind = Kind::Reject;
                } else if (pending->size() < kTerminalStreamHeaderSize) {
                    return;
                } else {
                    TerminalStreamHeader header;
                    std::string header_error;
                    const size_t excess = pending->size() - kTerminalStreamHeaderSize;
                    if (!decode_terminal_stream_header(pending->data(),
                                                       kTerminalStreamHeaderSize, header,
                                                       &header_error)) {
                        kind = Kind::Bad;
                    } else {
                        const auto found = terminal_entries.find(header.terminal_id);
                        TerminalEntry* entry =
                            found == terminal_entries.end() ? nullptr : &found->second;
                        if (!entry && role == Role::Client) {
                            // The host's downlink can overtake TerminalResult:
                            // control and CTTY travel on different streams and
                            // QUIC does not order across streams. A downlink
                            // for a terminal this side just asked for proves
                            // the host created it, so accept it now; the
                            // result still arrives and unlocks the input
                            // direction.
                            const auto requested =
                                client_pending_terminals.find(header.terminal_id);
                            if (requested != client_pending_terminals.end()) {
                                TerminalEntry created;
                                created.terminal_id = header.terminal_id;
                                created.server_side = false;
                                created.columns = requested->second.columns;
                                created.rows = requested->second.rows;
                                entry = &terminal_entries
                                             .emplace(header.terminal_id, std::move(created))
                                             .first->second;
                            }
                        }
                        HQUIC* slot = nullptr;
                        if (entry && !entry->failed &&
                            entry->server_side == (role == Role::Server)) {
                            slot = role == Role::Server ? &entry->uplink : &entry->downlink;
                        }
                        if (!slot || *slot || excess > kTerminalReceiveHighWaterBytes) {
                            // A duplicate or unknown direction is a viewer
                            // bug; refusing it quietly keeps the console.
                            kind = Kind::Reject;
                        } else {
                            *slot = stream;
                            if (excess > 0) {
                                entry->received.emplace_back(
                                    pending->begin() + kTerminalStreamHeaderSize,
                                    pending->end());
                                entry->buffered_bytes += excess;
                            }
                            if (entry->buffered_bytes >= kTerminalReceiveHighWaterBytes) {
                                entry->receive_paused = true;
                                terminal_pause = stream;
                            }
                            kind = Kind::Terminal;
                        }
                    }
                }
            } else if (std::memcmp(pending->data(), kCameraStreamMagic, 4) == 0) {
                // Host to viewer only. The stream can overtake the CameraResult
                // that granted it, so both the pending and the running id name
                // an acceptable stream; anything else is refused quietly.
                const bool expecting =
                    client_pending_camera != 0 || client_camera_id != 0;
                if (role != Role::Client ||
                    (effective_permissions() & kPermissionCamera) == 0) {
                    kind = Kind::Reject;
                } else if (pending->size() < kCameraStreamHeaderSize) {
                    return;
                } else {
                    CameraStreamHeader header;
                    std::string header_error;
                    if (!decode_camera_stream_header(pending->data(),
                                                     kCameraStreamHeaderSize, header,
                                                     &header_error)) {
                        kind = Kind::Bad;
                    } else if (!expecting ||
                               (header.camera_id != client_pending_camera &&
                                header.camera_id != client_camera_id)) {
                        kind = Kind::Reject;
                    } else if (camera.load() != nullptr && !camera_finished) {
                        // One camera per session; a second stream is a host bug.
                        kind = Kind::Reject;
                    } else {
                        camera = stream;
                        camera_finished = false;
                        client_camera_id = header.camera_id;
                        client_pending_camera = 0;
                        camera_parser.reset();
                        camera_received.clear();
                        if (pending->size() > kCameraStreamHeaderSize) {
                            camera_received.emplace(
                                pending->data() + kCameraStreamHeaderSize,
                                pending->data() + pending->size());
                        }
                        kind = Kind::Camera;
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
            // Disable under the same lock the consumers take before they
            // re-enable, so a drain that happens right now cannot be undone
            // by a late disable that leaves the stream stalled.
            if (port_forward_pause) {
                // The host dials before any byte reaches the relay; re-enabled
                // by accept_port_forward_connection().
                api->StreamReceiveSetEnabled(port_forward_pause, FALSE);
            }
            if (terminal_pause) {
                // A first packet that already filled the queue; re-enabled by
                // the application once the queue drained below half.
                api->StreamReceiveSetEnabled(terminal_pause, FALSE);
            }
        }
        for (const auto& message : screenshot_messages) {
            if (message.first != ControlType::ScreenshotResponse) {
                api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                break;
            }
            handle_control(message.first, message.second);
        }
        if (kind == Kind::Reject) {
            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            if (file_rejection) {
                // The viewer matches the result to its upload and fails the
                // attempt explicitly instead of seeing only a stream abort.
                // The control handle is used under the same lock that guards
                // its lifetime.
                std::lock_guard<std::mutex> lock(mutex);
                if (HQUIC handle = control.load()) {
                    try_send_bytes(handle, make_file_result(*file_rejection),
                                   QUIC_SEND_FLAG_NONE);
                }
            }
        } else if (kind == Kind::Bad) {
            note_transport_failure("the peer opened an unusable stream");
        }
        cv.notify_all();
    }

    // Fails one upload without ending the session: queued bytes are dropped,
    // the stream is aborted, and the host loop reports the reason.
    void fail_file_stream_locked(ReceivedUpload& upload, uint32_t id,
                                 const std::string& error) {
        upload.finished = true;
        upload.error = error;
        drop_file_chunks_locked(id);
        if (upload.stream) {
            api->StreamShutdown(upload.stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        }
    }

    // Turns CFIL body bytes into file chunks. v1 is raw content; v2 carries
    // 9-byte block headers, and a block may hold zstd data instead of raw
    // bytes. A block can straddle QUIC receive buffers, so an incomplete tail
    // stays in block_pending until the rest arrives.
    void feed_file_stream_locked(ReceivedUpload& upload, uint32_t id, const uint8_t* data,
                                 size_t size) {
        if (upload.finished) {
            return;
        }
        upload.wire_bytes += size;
        if (!upload.compressed) {
            push_file_chunk_locked(upload, id, data, size,
                                   upload.start_offset + upload.received);
            return;
        }
        upload.block_pending.insert(upload.block_pending.end(), data, data + size);
        size_t read = 0;
        while (upload.block_pending.size() - read >= kFileBlockHeaderSize) {
            FileBlockHeader header;
            std::string error;
            if (!decode_file_block_header(upload.block_pending.data() + read,
                                          kFileBlockHeaderSize, header, &error)) {
                fail_file_stream_locked(upload, id, "bad compressed file block: " + error);
                return;
            }
            const size_t total = kFileBlockHeaderSize + header.stored_size;
            if (upload.block_pending.size() - read < total) {
                break;
            }
            const uint8_t* payload =
                upload.block_pending.data() + read + kFileBlockHeaderSize;
            if (!header.compressed) {
                push_file_chunk_locked(upload, id, payload, header.raw_size,
                                       upload.start_offset + upload.received);
            } else {
                std::vector<uint8_t> raw(header.raw_size);
                ZSTD_DCtx* context = zstd_contexts().decompressor;
                const size_t produced =
                    context ? ZSTD_decompressDCtx(context, raw.data(), raw.size(), payload,
                                                  header.stored_size)
                            : 0;
                if (!context || ZSTD_isError(produced) || produced != header.raw_size) {
                    fail_file_stream_locked(upload, id,
                                            "compressed file block did not decompress");
                    return;
                }
                push_file_chunk_locked(upload, id, raw.data(), raw.size(),
                                       upload.start_offset + upload.received);
            }
            if (upload.finished) {
                return;
            }
            read += total;
        }
        if (read == upload.block_pending.size()) {
            upload.block_pending.clear();
        } else if (read != 0) {
            upload.block_pending.erase(upload.block_pending.begin(),
                                       upload.block_pending.begin() + read);
        }
    }

    // Caller holds `mutex` and an entry in received_uploads. Drops every queued
    // byte of the transfer when the receive window would overflow and marks
    // the upload as broken; the host then reports IoError.
    void push_file_chunk_locked(ReceivedUpload& upload, uint32_t id, const uint8_t* data,
                                size_t size, uint64_t offset) {
        if (upload.finished) {
            return;
        }
        // A shared policy may reduce the atomic grant before the host loop
        // reaches its stream cleanup. Do not buffer bytes during that gap.
        if (!file_allowed(upload.clipboard, false)) {
            upload.finished = true;
            upload.error = "file or clipboard permission was disabled";
            drop_file_chunks_locked(id);
            if (upload.stream) api->StreamShutdown(upload.stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
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

    // Caller holds `mutex`. The receive stream of a forwarded connection: the
    // viewer reads the downlink, the host reads the uplink.
    static HQUIC port_forward_receive_stream(const PortForwardEntry& entry) {
        return entry.server_side ? entry.uplink : entry.downlink;
    }

    static HQUIC port_forward_send_stream(const PortForwardEntry& entry) {
        return entry.server_side ? entry.downlink : entry.uplink;
    }

    PortForwardEntry* port_forward_by_stream_locked(HQUIC stream) {
        for (auto& item : port_forward_connections) {
            if (item.second.uplink == stream || item.second.downlink == stream) {
                return &item.second;
            }
        }
        return nullptr;
    }

    PortForwardEntry* port_forward_by_handle_locked(PortForwardHandle handle) {
        const auto it = port_forward_connections.find(handle);
        return it == port_forward_connections.end() ? nullptr : &it->second;
    }

    // Caller holds `mutex`. Finished entries stay until the application closes
    // the handle: the tail of the receive queue may still be unread, and
    // receive() reports Unavailable after it instead of a vanished handle.
    // They do not use the connection budget.
    size_t port_forward_live_count_locked() const {
        size_t live = 0;
        for (const auto& item : port_forward_connections) {
            if (item.second.app_closed) {
                continue;
            }
            if (item.second.uplink != nullptr || item.second.downlink != nullptr) {
                ++live;
            }
        }
        return live;
    }

    // Caller holds `mutex`. Forgets every finished entry; called before a new
    // connection tries to claim the last slot.
    void prune_retired_port_forwards_locked() {
        for (auto it = port_forward_connections.begin();
             it != port_forward_connections.end();) {
            if (it->second.uplink == nullptr && it->second.downlink == nullptr) {
                it = port_forward_connections.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Caller holds `mutex`. Marks the connection broken and drops its queued
    // bytes; the peer stream is aborted by the caller, which may hold a
    // handle the callbacks no longer own.
    void fail_port_forward_locked(PortForwardEntry& entry, const std::string& error,
                                  std::vector<HQUIC>& aborts) {
        entry.failed = true;
        entry.error = error;
        entry.receive_finished = true;
        entry.send_finished = true;
        entry.buffered_bytes = 0;
        entry.received.clear();
        if (entry.uplink) {
            aborts.push_back(entry.uplink);
        }
        if (entry.downlink) {
            aborts.push_back(entry.downlink);
        }
    }

    // Caller holds `mutex`. Tears down every connection under one target and
    // forgets the registrations; returns the stream handles to abort.
    std::vector<HQUIC> stop_port_forward_target_locked(uint32_t forward_id) {
        std::vector<HQUIC> aborts;
        for (auto it = port_forward_connections.begin(); it != port_forward_connections.end();) {
            if (it->second.forward_id != forward_id) {
                ++it;
                continue;
            }
            fail_port_forward_locked(it->second, "the forward target was stopped", aborts);
            it = port_forward_connections.erase(it);
        }
        active_forward_targets.erase(forward_id);
        client_forward_targets.erase(forward_id);
        client_pending_forwards.erase(forward_id);
        pending_forward_targets.erase(forward_id);
        port_forward_requests.erase(
            std::remove_if(port_forward_requests.begin(), port_forward_requests.end(),
                           [forward_id](const PortForwardRequest& request) {
                               return request.forward_id == forward_id;
                           }),
            port_forward_requests.end());
        return aborts;
    }

    // Caller holds `mutex`. Revocation path: no target survives, and every
    // live connection goes back to the caller to abort.
    std::vector<HQUIC> clear_port_forward_locked(const std::string& reason) {
        std::vector<HQUIC> aborts;
        for (auto& item : port_forward_connections) {
            fail_port_forward_locked(item.second, reason, aborts);
        }
        port_forward_connections.clear();
        active_forward_targets.clear();
        client_forward_targets.clear();
        client_pending_forwards.clear();
        pending_forward_targets.clear();
        port_forward_requests.clear();
        port_forward_results.clear();
        port_forward_opens.clear();
        return aborts;
    }

    // Caller holds `mutex`. Everything a session reset has to forget; the
    // handles go back to the caller for closing.
    std::vector<HQUIC> detach_port_forward_locked() {
        std::vector<HQUIC> handles;
        for (const auto& item : port_forward_connections) {
            if (item.second.uplink) {
                handles.push_back(item.second.uplink);
            }
            if (item.second.downlink) {
                handles.push_back(item.second.downlink);
            }
        }
        port_forward_connections.clear();
        active_forward_targets.clear();
        client_forward_targets.clear();
        client_pending_forwards.clear();
        pending_forward_targets.clear();
        port_forward_requests.clear();
        port_forward_results.clear();
        port_forward_opens.clear();
        port_forward_stops.clear();
        next_port_forward_handle = 1;
        return handles;
    }

    bool has_port_forward_entry_locked(uint32_t forward_id, uint32_t connection_id) const {
        for (const auto& item : port_forward_connections) {
            if (item.second.forward_id == forward_id &&
                item.second.connection_id == connection_id) {
                return true;
            }
        }
        return false;
    }

    // Caller holds `mutex`. The viewer reads the downlink, the host reads the
    // uplink; the same split as a forwarded TCP connection.
    static HQUIC terminal_receive_stream(const TerminalEntry& entry) {
        return entry.server_side ? entry.uplink : entry.downlink;
    }

    static HQUIC terminal_send_stream(const TerminalEntry& entry) {
        return entry.server_side ? entry.downlink : entry.uplink;
    }

    TerminalEntry* terminal_by_stream_locked(HQUIC stream) {
        for (auto& item : terminal_entries) {
            if (item.second.uplink == stream || item.second.downlink == stream) {
                return &item.second;
            }
        }
        return nullptr;
    }

    // Caller holds `mutex`. Forgets one terminal and every trace of it in the
    // request queues; the stream handles go back to the caller for closing.
    // `keep_finished` leaves a direction that already ended cleanly alone, so
    // a stop sent after the data stream's FIN cannot turn into a reset that
    // makes the peer discard output still queued on its side.
    std::vector<HQUIC> drop_terminal_locked(uint32_t terminal_id,
                                           bool keep_finished = false) {
        std::vector<HQUIC> aborts;
        const auto it = terminal_entries.find(terminal_id);
        if (it != terminal_entries.end()) {
            if (it->second.uplink && !(keep_finished && it->second.receive_finished)) {
                aborts.push_back(it->second.uplink);
            }
            if (it->second.downlink && !(keep_finished && it->second.send_finished)) {
                aborts.push_back(it->second.downlink);
            }
            terminal_entries.erase(it);
        }
        pending_terminal_targets.erase(terminal_id);
        active_terminal_targets.erase(terminal_id);
        client_pending_terminals.erase(terminal_id);
        client_terminal_targets.erase(terminal_id);
        terminal_requests.erase(
            std::remove_if(terminal_requests.begin(), terminal_requests.end(),
                           [terminal_id](const TerminalRequest& request) {
                               return request.terminal_id == terminal_id;
                           }),
            terminal_requests.end());
        terminal_resizes.erase(
            std::remove_if(terminal_resizes.begin(), terminal_resizes.end(),
                           [terminal_id](const TerminalSize& size) {
                               return size.terminal_id == terminal_id;
                           }),
            terminal_resizes.end());
        return aborts;
    }

    // Caller holds `mutex`. One direction died, so the terminal as a whole is
    // over: the other direction is aborted and the application learns from
    // take_terminal_output()/take_terminal_input() that the console ended.
    void fail_terminal_locked(TerminalEntry& entry, const std::string& error,
                              std::vector<HQUIC>& aborts) {
        entry.failed = true;
        entry.error = error;
        entry.receive_finished = true;
        entry.send_finished = true;
        entry.buffered_bytes = 0;
        entry.received.clear();
        if (entry.uplink) {
            aborts.push_back(entry.uplink);
        }
        if (entry.downlink) {
            aborts.push_back(entry.downlink);
        }
    }

    // Caller holds `mutex`. Revocation path: every terminal ends, and the
    // local application gets a stop per terminal so it closes the console and
    // the child process it owns.
    std::vector<HQUIC> clear_terminals_locked(const std::string& reason) {
        std::vector<HQUIC> aborts;
        for (const auto& item : terminal_entries) {
            TerminalStop stop;
            stop.terminal_id = item.first;
            stop.code = TerminalCode::Cancelled;
            stop.reason = reason;
            while (terminal_stops.size() >= 8) {
                terminal_stops.pop_front();
            }
            terminal_stops.push_back(std::move(stop));
            if (item.second.uplink) {
                aborts.push_back(item.second.uplink);
            }
            if (item.second.downlink) {
                aborts.push_back(item.second.downlink);
            }
        }
        terminal_entries.clear();
        pending_terminal_targets.clear();
        active_terminal_targets.clear();
        client_pending_terminals.clear();
        client_terminal_targets.clear();
        terminal_requests.clear();
        terminal_results.clear();
        terminal_resizes.clear();
        return aborts;
    }

    // Caller holds `mutex`. Everything a session reset has to forget; the
    // handles go back to the caller for closing.
    std::vector<HQUIC> detach_terminal_locked() {
        std::vector<HQUIC> handles;
        for (const auto& item : terminal_entries) {
            if (item.second.uplink) {
                handles.push_back(item.second.uplink);
            }
            if (item.second.downlink) {
                handles.push_back(item.second.downlink);
            }
        }
        terminal_entries.clear();
        pending_terminal_targets.clear();
        active_terminal_targets.clear();
        client_pending_terminals.clear();
        client_terminal_targets.clear();
        terminal_requests.clear();
        terminal_results.clear();
        terminal_resizes.clear();
        terminal_stops.clear();
        return handles;
    }

    // Appends data of a stream that was already classified as CCLB or CFIL.
    // Returns false when the handle belongs to neither, so the caller can
    // classify it from its first bytes.
    bool route_bulk_stream(HQUIC stream, QUIC_STREAM_EVENT& event) {
        bool screenshot = false, valid = true;
        std::vector<std::pair<ControlType, std::vector<uint8_t>>> messages;
        {
            std::lock_guard lock(mutex);
            for (auto& entry : screenshot_streams) {
                if (entry.stream != stream) continue;
                screenshot = true;
                std::string error;
                for (uint32_t i = 0; i < event.RECEIVE.BufferCount && valid; ++i) {
                    const auto& buffer = event.RECEIVE.Buffers[i];
                    valid = entry.parser.push(buffer.Buffer, buffer.Length, messages, &error);
                }
                break;
            }
        }
        if (screenshot) {
            for (const auto& message : messages) {
                if (message.first != ControlType::ScreenshotResponse) { valid = false; break; }
                handle_control(message.first, message.second);
            }
            if (!valid) api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            return true;
        }
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
                        // abort this stream only and keep the last item. The
                        // aggregate limit is checked on every push because the
                        // classification-time check sees only the first chunk
                        // and kMaxClipboardStreams concurrent streams can
                        // otherwise exceed it many times over.
                        if (entry.bytes.size() + buffer.Length > kMaxClipboardStreamBytes ||
                            pending_clipboard_bytes + buffer.Length >
                                kMaxPendingClipboardBytes) {
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
                    api->StreamReceiveSetEnabled(stream, FALSE);
                    return true;
                }
                for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                    feed_file_stream_locked(entry.second, entry.first, buffer.Buffer,
                                            buffer.Length);
                }
                return true;
            }
            PortForwardEntry* forward = port_forward_by_stream_locked(stream);
            if (forward) {
                if (stream != port_forward_receive_stream(*forward) ||
                    forward->receive_finished) {
                    // A retired handle or the sending half; nothing to queue.
                    return true;
                }
                // A receive event is accepted whole or not at all, so the
                // queue stops at high water and the local socket's pace
                // reaches the peer through QUIC flow control. Aborting here
                // would break a healthy relay whose far end is simply busy.
                if (forward->receive_paused ||
                    forward->buffered_bytes >= kPortForwardReceiveHighWaterBytes) {
                    forward->receive_paused = true;
                    event.RECEIVE.TotalBufferLength = 0;
                    api->StreamReceiveSetEnabled(stream, FALSE);
                    return true;
                }
                for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                    forward->received.emplace_back(buffer.Buffer, buffer.Buffer + buffer.Length);
                    forward->buffered_bytes += buffer.Length;
                }
                if (forward->buffered_bytes >= kPortForwardReceiveHighWaterBytes) {
                    forward->receive_paused = true;
                    api->StreamReceiveSetEnabled(stream, FALSE);
                }
                cv.notify_all();
                return true;
            }
            TerminalEntry* terminal = terminal_by_stream_locked(stream);
            if (terminal) {
                if (stream != terminal_receive_stream(*terminal) ||
                    terminal->receive_finished || terminal->failed) {
                    return true;
                }
                // Same pause contract as a forwarded connection: a receive
                // event is taken whole or not at all, so the console's reader
                // thread sets the pace through QUIC flow control.
                if (terminal->receive_paused ||
                    terminal->buffered_bytes >= kTerminalReceiveHighWaterBytes) {
                    terminal->receive_paused = true;
                    event.RECEIVE.TotalBufferLength = 0;
                    api->StreamReceiveSetEnabled(stream, FALSE);
                    return true;
                }
                for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                    terminal->received.emplace_back(buffer.Buffer, buffer.Buffer + buffer.Length);
                    terminal->buffered_bytes += buffer.Length;
                }
                if (terminal->buffered_bytes >= kTerminalReceiveHighWaterBytes) {
                    terminal->receive_paused = true;
                    api->StreamReceiveSetEnabled(stream, FALSE);
                }
                cv.notify_all();
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
        for (const auto& entry : screenshot_streams) handles.push_back(entry.stream);
        screenshot_streams.clear();
        outgoing_screenshot = nullptr;
        outgoing_screenshot_request = 0;
        outgoing_screenshot_next = 0;
        clipboard_streams.clear();
        pending_clipboard_bytes = 0;
        clipboard_contents.clear();
        clipboard_contents_seen = 0;
        clipboard_file_reads.clear(); clipboard_file_data.clear();
        requested_clipboard_reads.clear(); accepted_clipboard_reads.clear();
        file_offers.clear();
        file_offers_seen = 0;
        file_chunks.clear();
        buffered_file_bytes = 0;
        received_uploads.clear();
        incoming_file_offers.clear();
        file_cancels.clear();
        outgoing_uploads.clear();
        file_answers.clear();
        file_results.clear();
        file_list_requests.clear();
        file_list_results.clear();
        file_download_requests.clear();
        file_download_results.clear();
        file_op_requests.clear();
        file_op_results.clear();
        one_shot_streams.clear();
        std::vector<HQUIC> forwards = detach_port_forward_locked();
        handles.insert(handles.end(), forwards.begin(), forwards.end());
        std::vector<HQUIC> terminals = detach_terminal_locked();
        handles.insert(handles.end(), terminals.begin(), terminals.end());
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
                if (stream == video.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (queue_media_locked(received, event, kVideoReceiveHighWaterBytes)) {
                        api->StreamReceiveSetEnabled(stream, FALSE);
                    }
                    cv.notify_all();
                } else if (stream == audio.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (queue_media_locked(audio_received, event,
                                           kAudioReceiveHighWaterBytes)) {
                        api->StreamReceiveSetEnabled(stream, FALSE);
                    }
                    cv.notify_all();
                } else if (stream == microphone.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (queue_media_locked(microphone_received, event,
                                           kMicrophoneReceiveHighWaterBytes)) {
                        api->StreamReceiveSetEnabled(stream, FALSE);
                    }
                    cv.notify_all();
                } else if (stream == camera.load()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    // After a local stop the rest of the host's in-flight
                    // frames are dropped instead of buffered for nobody.
                    if (!camera_finished &&
                        queue_media_locked(camera_received, event,
                                           kCameraReceiveHighWaterBytes)) {
                        api->StreamReceiveSetEnabled(stream, FALSE);
                    }
                    cv.notify_all();
                } else if (stream == control.load()) {
                    std::vector<std::pair<ControlType, std::vector<uint8_t>>> messages;
                    std::string error;
                    bool ok = true;
                    {
                        // reset_session_locked() clears the parser under this
                        // lock while a reconnect is being prepared; the stream
                        // callback must not push framing state into it at the
                        // same time.
                        std::lock_guard<std::mutex> lock(mutex);
                        for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                            const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                            if (!control_parser.push(buffer.Buffer, buffer.Length, messages,
                                                     &error)) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    if (!ok) {
                        control_protocol_failure("control stream: " + error);
                        break;
                    }
                    for (const auto& message : messages) {
                        handle_control(message.first, message.second);
                    }
                } else if (!route_bulk_stream(stream, event)) {
                    classify_media_stream(stream, event);
                }
                break;
            }
            case QUIC_STREAM_EVENT_SEND_COMPLETE: {
                auto* pending = static_cast<PendingSend*>(event.SEND_COMPLETE.ClientContext);
                if (pending) {
                    release_send(pending->bytes.size(), pending->generation, pending->screenshot);
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
                    } else if (stream == camera.load()) {
                        camera_finished = true;
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
                            const bool allowed = clipboard_allowed(false);
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
                                    if (entry.second.compressed &&
                                        !entry.second.block_pending.empty()) {
                                        entry.second.error =
                                            "the compressed upload ended in the middle of a "
                                            "block";
                                        drop_file_chunks_locked(entry.first);
                                    }
                                    matched = true;
                                    break;
                                }
                            }
                        }
                        if (!matched) {
                            PortForwardEntry* forward =
                                port_forward_by_stream_locked(stream);
                            if (forward && stream == port_forward_receive_stream(*forward)) {
                                forward->receive_finished = true;
                            }
                        }
                        if (!matched) {
                            TerminalEntry* terminal = terminal_by_stream_locked(stream);
                            if (terminal && stream == terminal_receive_stream(*terminal)) {
                                terminal->receive_finished = true;
                            }
                        }
                    }
                }
                cv.notify_all();
                break;
            }
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED: {
                bool fatal = false;
                bool control_abort = false;
                std::vector<HQUIC> bulk_aborts;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    bool bulk = std::any_of(screenshot_streams.begin(), screenshot_streams.end(),
                        [stream](const auto& entry) { return entry.stream == stream; }) ||
                        std::find(one_shot_streams.begin(), one_shot_streams.end(), stream) != one_shot_streams.end();
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
                    if (!bulk) {
                        PortForwardEntry* forward = port_forward_by_stream_locked(stream);
                        if (forward && stream == port_forward_receive_stream(*forward)) {
                            // One direction died; the TCP connection is gone.
                            // Tell the peer by aborting the other direction
                            // instead of leaving it half-open.
                            fail_port_forward_locked(*forward,
                                                     "the peer aborted the port forward connection",
                                                     bulk_aborts);
                            bulk = true;
                        }
                    }
                    if (!bulk) {
                        TerminalEntry* terminal = terminal_by_stream_locked(stream);
                        if (terminal && stream == terminal_receive_stream(*terminal)) {
                            fail_terminal_locked(*terminal,
                                                 "the peer aborted the terminal stream",
                                                 bulk_aborts);
                            bulk = true;
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
                    } else if (stream == camera.load()) {
                        // The camera is optional: a broken stream ends the
                        // camera, not the desktop session.
                        camera_finished = true;
                    } else if (stream == control.load()) {
                        control_abort = true;
                    } else if (!bulk) {
                        fatal = true;
                    }
                    abort_streams_locked(bulk_aborts, stream);
                }
                if (control_abort) {
                    control_protocol_failure("QUIC peer aborted the control stream");
                } else if (fatal) {
                    note_transport_failure("QUIC peer aborted a stream");
                }
                cv.notify_all();
                break;
            }
            case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED: {
                // The peer sent STOP_SENDING for the direction this side
                // writes. For a forwarded connection that is a broken TCP
                // path; the other direction is aborted so both apps see it.
                std::vector<HQUIC> aborts;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    PortForwardEntry* forward = port_forward_by_stream_locked(stream);
                    if (forward && stream == port_forward_send_stream(*forward)) {
                        fail_port_forward_locked(
                            *forward, "the peer stopped reading the port forward stream",
                            aborts);
                    }
                    TerminalEntry* terminal = terminal_by_stream_locked(stream);
                    if (terminal && stream == terminal_send_stream(*terminal)) {
                        fail_terminal_locked(*terminal,
                                             "the peer stopped reading the terminal stream",
                                             aborts);
                    }
                    if (stream == camera.load()) {
                        // The viewer no longer reads the camera; the host's
                        // capture loop sees the same end through camera_open().
                        camera_finished = true;
                    }
                    abort_streams_locked(aborts, stream);
                }
                cv.notify_all();
                break;
            }
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
                std::vector<HQUIC> to_close;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (auto it = screenshot_streams.begin(); it != screenshot_streams.end(); ++it) {
                        if (it->stream == stream) {
                            screenshot_streams.erase(it);
                            to_close.push_back(stream);
                            break;
                        }
                    }
                    if (outgoing_screenshot == stream) outgoing_screenshot = nullptr;
                    if (stream == video.load()) {
                        video_shutdown_complete = true;
                    } else if (stream == audio.load()) {
                        audio_shutdown_complete = true;
                    } else if (stream == control.load()) {
                        control_shutdown_complete = true;
                    } else if (stream == camera.load()) {
                        camera = nullptr;
                        camera_finished = true;
                        to_close.push_back(stream);
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
                            PortForwardEntry* forward =
                                port_forward_by_stream_locked(stream);
                            if (forward) {
                                const bool receive_side =
                                    stream == port_forward_receive_stream(*forward);
                                if (receive_side) {
                                    forward->receive_finished = true;
                                } else {
                                    forward->send_finished = true;
                                }
                                if (forward->uplink == stream) {
                                    forward->uplink = nullptr;
                                }
                                if (forward->downlink == stream) {
                                    forward->downlink = nullptr;
                                }
                                forward->receive_paused = false;
                                to_close.push_back(stream);
                                // Both halves are gone: the entry is finished,
                                // but it stays readable (and reports
                                // Unavailable once drained) until the
                                // application closes the handle.
                            }
                            TerminalEntry* terminal = terminal_by_stream_locked(stream);
                            if (terminal) {
                                if (stream == terminal_receive_stream(*terminal)) {
                                    terminal->receive_finished = true;
                                } else {
                                    terminal->send_finished = true;
                                }
                                if (terminal->uplink == stream) {
                                    terminal->uplink = nullptr;
                                }
                                if (terminal->downlink == stream) {
                                    terminal->downlink = nullptr;
                                }
                                terminal->receive_paused = false;
                                to_close.push_back(stream);
                                // The entry outlives its streams so the
                                // application can drain the queue; drop_terminal
                                // or close_terminal forgets it.
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
                    control_protocol_failure("Hello: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    requested_display = hello.display;
                    decoding_codecs = hello.decoding_codecs;
                    decoding_444 = hello.decoding_444;
                    initial_chroma_preference = hello.prefer_chroma;
                    initial_codec_preference = hello.codec_preference;
                    requested_mode = hello.mode;
                    requested_peer = remote_address();
                    session_audit_ref = hello.audit_ref;
                    client_audio_datagram.store(
                        (hello.capabilities & kClientCapabilityAudioDatagram) != 0);
                    pending_hello = std::move(hello);
                    hello_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::AuthBegin: {
                if (role != Role::Server) {
                    break;
                }
                if (!payload.empty()) {
                    control_protocol_failure("AuthBegin carries a payload");
                    break;
                }
                std::array<uint8_t, kTicketProofHostNonceSize> nonce{};
                bool issued = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (host_nonce_sent) {
                        // A repeated AuthBegin gets the same challenge: it
                        // binds nothing, and a fresh value would break a
                        // proof the viewer is already signing.
                        nonce = host_nonce;
                        issued = true;
                    }
                }
                if (!issued) {
                    if (!secure_random(nonce.data(), nonce.size())) {
                        control_protocol_failure(
                            "cannot generate the authentication challenge");
                        break;
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    host_nonce = nonce;
                    host_nonce_sent = true;
                }
                if (!try_send_bytes(control.load(), make_auth_challenge(nonce),
                                    QUIC_SEND_FLAG_NONE)) {
                    control_protocol_failure("cannot send the authentication challenge");
                }
                break;
            }
            case ControlType::AuthChallenge: {
                if (role != Role::Client) {
                    break;
                }
                std::array<uint8_t, kTicketProofHostNonceSize> nonce{};
                if (!parse_auth_challenge(payload, nonce, &error)) {
                    control_protocol_failure("AuthChallenge: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    host_nonce = nonce;
                    auth_challenge_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::TicketRenew: {
                if (role != Role::Server) {
                    break;
                }
                TicketRenewRequest request;
                if (!parse_ticket_renew(payload, request, &error)) {
                    control_protocol_failure("TicketRenew: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ticket_renew = std::move(request);
                    ticket_renew_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::TicketRenewResult: {
                if (role != Role::Client) {
                    break;
                }
                TicketRenewResult result;
                if (!parse_ticket_renew_result(payload, result, &error)) {
                    control_protocol_failure("TicketRenewResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ticket_renew_result = std::move(result);
                    ticket_renew_result_ready = true;
                    ++ticket_renew_results_seen;
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
                    control_protocol_failure("AccessDenied: " + error);
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
                    control_protocol_failure("HelloAck: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_permissions = permission_mask_from_info(info);
                    peer_clipboard_direction = info.clipboard_direction;
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
                    control_protocol_failure("StreamInfo: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_permissions = permission_mask_from_info(info);
                    peer_clipboard_direction = info.clipboard_direction;
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
                    control_protocol_failure("Stats: " + error);
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
                    (effective_permissions() & kPermissionInput) == 0) {
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
                    control_protocol_failure("InputEvent: " + error);
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
            case ControlType::TextInput: {
                if (role != Role::Server ||
                    (effective_permissions() & kPermissionInput) == 0) {
                    // Composed text is typing, so it follows the same input
                    // permission as InputEvent, including mid-session
                    // revocation.
                    break;
                }
                std::string text;
                if (!parse_text_input(payload, text, &error)) {
                    control_protocol_failure("TextInput: " + error);
                    break;
                }
                if (injector) {
                    injector->submit_text(text);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (texts.size() >= 64) {
                        texts.pop_front();
                    }
                    texts.push_back(std::move(text));
                    ++texts_seen;
                }
                cv.notify_all();
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
            case ControlType::VideoReceipt: {
                if (role != Role::Server || !hello_complete) break;
                uint64_t sequence = 0;
                if (!parse_video_receipt(payload, sequence, &error)) {
                    control_protocol_failure("VideoReceipt: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // Duplicate, stale and future receipts cannot erase the
                    // outstanding window or manufacture a delay sample. The
                    // newest send may be an untracked refinement, so the bound
                    // is the last sequence handed out, not the last windowed
                    // entry: stream order still proves everything older was
                    // delivered.
                    if (pending_video.empty() || sequence < pending_video.front().sequence ||
                        sequence >= next_seq.load()) break;
                    const auto now = steady_us();
                    while (!pending_video.empty() && pending_video.front().sequence <= sequence) {
                        latency.video_delay_us = now - pending_video.front().capture_us;
                        pending_video_bytes -= pending_video.front().bytes;
                        pending_video.pop_front();
                    }
                    ++latency.video_replies;
                }
                cv.notify_all();
                break;
            }
            case ControlType::LatencyProbe: {
                if (!hello_complete) {
                    break;
                }
                uint64_t client_us = 0;
                if (!parse_latency_probe(payload, client_us, &error)) {
                    control_protocol_failure("LatencyProbe: " + error);
                    break;
                }
                const uint64_t recv_us = steady_us();
                const std::vector<uint8_t> reply =
                    make_latency_reply(client_us, recv_us, steady_us());
                // A full send queue drops this sample; the viewer probes again
                // in a second. Blocking a MsQuic worker here would be worse.
                try_send_bytes(control.load(), reply, QUIC_SEND_FLAG_NONE);
                if (role == Role::Client) flush_video_receipt();
                break;
            }
            case ControlType::LatencyReply: {
                uint64_t client_us = 0;
                uint64_t host_recv_us = 0;
                uint64_t host_send_us = 0;
                if (!parse_latency_reply(payload, client_us, host_recv_us, host_send_us,
                                         &error)) {
                    control_protocol_failure("LatencyReply: " + error);
                    break;
                }
                const uint64_t received_us = steady_us();
                if (role == Role::Server) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (pending_qos_probe_us && client_us == pending_qos_probe_us &&
                        received_us >= client_us) {
                        latency.valid = true;
                        latency.rtt_us = received_us - client_us;
                        ++latency.replies;
                        pending_qos_probe_us = 0;
                    }
                    break;
                }
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
                PermissionMask permissions = 0;
                if (!parse_permission_update(payload, permissions, &error)) {
                    control_protocol_failure("PermissionUpdate: " + error);
                    break;
                }
                std::vector<HQUIC> aborts;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_permissions = permissions;
                    permission_update_ready = true;
                    ++permission_updates;
                    if ((permissions & kPermissionClipboard) == 0) {
                        clipboard_contents.clear();
                        clipboard_updates.clear();
                        clipboard_file_reads.clear(); clipboard_file_data.clear();
                        requested_clipboard_reads.clear(); accepted_clipboard_reads.clear();
                    }
                    revoke_file_permissions_locked();
                    if ((permissions & kPermissionPortForward) == 0) {
                        aborts = clear_port_forward_locked("port forwarding was revoked");
                    }
                    if ((permissions & kPermissionTerminal) == 0) {
                        std::vector<HQUIC> terminals =
                            clear_terminals_locked("terminal access was revoked");
                        aborts.insert(aborts.end(), terminals.begin(), terminals.end());
                    }
                    if ((permissions & kPermissionMicrophone) == 0 &&
                        (client_voice_active || client_voice_requested)) {
                        // The host stops the capture on its side; the viewer
                        // stops expecting audio and reports the close once.
                        client_voice_active = false;
                        client_voice_requested = false;
                        client_voice_request_id = 0;
                        voice_closed_notice = true;
                        voice_audio.clear();
                        buffered_voice_bytes = 0;
                    }
                    abort_streams_locked(aborts);
                }
                cv.notify_all();
                break;
            }
            case ControlType::SwitchDisplay: {
                // Selecting a subscription does not change host input or topology.
                if (role != Role::Server || !hello_complete || requested_mode != SessionMode::Desktop) {
                    break;
                }
                uint16_t index = kDisplayAuto;
                if (!parse_switch_display(payload, index, &error)) {
                    control_protocol_failure("SwitchDisplay: " + error);
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
            case ControlType::CaptureDisplays: {
                if (role != Role::Server || !desktop_subscription_ready()) break;
                CaptureDisplaysRequest request;
                if (!parse_capture_displays(payload, request, &error)) {
                    control_protocol_failure("CaptureDisplays: " + error); break;
                }
                bool overflow = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    overflow = capture_display_requests.size() >= kMaxDisplaySubscriptionRequests;
                    if (!overflow) capture_display_requests.push_back(std::move(request));
                }
                if (overflow) control_protocol_failure("too many pending display subscription operations");
                cv.notify_all();
                break;
            }
            case ControlType::CaptureDisplaysResult: {
                if (role != Role::Client || !desktop_subscription_ready()) break;
                CaptureDisplaysResult result;
                if (!parse_capture_displays_result(payload, result, &error)) {
                    control_protocol_failure("CaptureDisplaysResult: " + error); break;
                }
                bool overflow = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    overflow = capture_display_results.size() >= kMaxDisplaySubscriptionRequests;
                    if (!overflow) {
                        subscribed_display_streams.clear();
                        display_stream_updates.clear(); // the complete result supersedes older updates
                        for (const auto& stream : result.streams)
                            subscribed_display_streams.emplace(stream.stream_id, stream.display_index);
                        for (auto it = display_cursor_updates.begin(); it != display_cursor_updates.end();)
                            if (!subscribed_display_streams.count(it->first)) it = display_cursor_updates.erase(it);
                            else ++it;
                        capture_display_results.push_back(std::move(result));
                    }
                }
                if (overflow) control_protocol_failure("too many unread display subscription results");
                cv.notify_all();
                break;
            }
            case ControlType::DisplayStreamInfo: {
                if (role != Role::Client || !desktop_subscription_ready()) break;
                DisplayStreamInfo info;
                if (!parse_display_stream_info(payload, info, &error)) {
                    control_protocol_failure("DisplayStreamInfo: " + error); break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto found = subscribed_display_streams.find(info.stream_id);
                    if (found != subscribed_display_streams.end() && found->second == info.display_index)
                        display_stream_updates[info.stream_id] = std::move(info);
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplayKeyframeRequest: {
                if (role != Role::Server || !desktop_subscription_ready()) break;
                uint32_t id = 0;
                if (!parse_display_keyframe_request(payload, id, &error)) {
                    control_protocol_failure("DisplayKeyframeRequest: " + error); break;
                }
                bool overflow = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (std::find(display_keyframe_requests.begin(), display_keyframe_requests.end(), id) ==
                        display_keyframe_requests.end()) {
                        overflow = display_keyframe_requests.size() >= kMaxDisplayCount;
                        if (!overflow) display_keyframe_requests.push_back(id);
                    }
                }
                if (overflow) control_protocol_failure("too many pending display keyframes");
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
                    control_protocol_failure("DisplaySwitchResult: " + error);
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
                    control_protocol_failure("ListDisplayModes: " + error);
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
                    control_protocol_failure("DisplayModes: " + error);
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
                    (effective_permissions() & kPermissionInput) == 0) {
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
                    control_protocol_failure("SetDisplayMode: " + error);
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
                    control_protocol_failure("DisplayModeResult: " + error);
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
            case ControlType::SetDisplayFollow: {
                if (role != Role::Server ||
                    (effective_permissions() & kPermissionInput) == 0) {
                    // Following moves the host's capture like SwitchDisplay, so
                    // it needs the same grant; without it the request is
                    // dropped rather than allowed to move the host's view.
                    break;
                }
                uint8_t flags = 0;
                if (!parse_set_display_follow(payload, flags, &error)) {
                    control_protocol_failure("SetDisplayFollow: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_follow_flags = flags;
                    display_follow_request_ready = true;
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplayFollowState: {
                if (role != Role::Client) {
                    break;
                }
                DisplayFollowState state;
                if (!parse_display_follow_state(payload, state, &error)) {
                    control_protocol_failure("DisplayFollowState: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_follow_state = std::move(state);
                    display_follow_state_ready = true;
                    ++display_follow_states_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::ShowCursor: {
                if (role != Role::Server) {
                    break;
                }
                ShowCursorHint hint;
                if (!parse_show_cursor(payload, hint, &error)) {
                    control_protocol_failure("ShowCursor: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    cursor_hint = hint;
                    cursor_hint_ready = true;
                    ++cursor_hints_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SetPrivacyMode: {
                if (role != Role::Server ||
                    (effective_permissions() & kPermissionPrivacyMode) == 0) {
                    // The HelloAck already told the viewer whether privacy mode
                    // is available; a request without the permission is
                    // dropped, not fatal.
                    break;
                }
                bool enabled = false;
                if (!parse_set_privacy_mode(payload, enabled, &error)) {
                    control_protocol_failure("SetPrivacyMode: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    privacy_mode_requested = enabled;
                    privacy_mode_request_ready = true;
                    ++privacy_mode_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::PrivacyModeState: {
                if (role != Role::Client) {
                    break;
                }
                bool enabled = false;
                PrivacyModeCode code = PrivacyModeCode::Failed;
                std::string reason;
                if (!parse_privacy_mode_state(payload, enabled, code, reason, &error)) {
                    control_protocol_failure("PrivacyModeState: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    privacy_mode_enabled = enabled;
                    privacy_mode_code = code;
                    privacy_mode_reason = std::move(reason);
                    privacy_mode_state_ready = true;
                    ++privacy_mode_states_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::DesktopState: {
                if (role != Role::Client) {
                    break;
                }
                DesktopKind kind = DesktopKind::Default;
                if (!parse_desktop_state(payload, kind, &error)) {
                    control_protocol_failure("DesktopState: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    desktop_kind = kind;
                    desktop_state_ready = true;
                    ++desktop_states_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SendSas: {
                if (role != Role::Server) {
                    break;
                }
                if (!parse_send_sas(payload, &error)) {
                    control_protocol_failure("SendSas: " + error);
                    break;
                }
                // Deliberately not gated on the permission mask here: the
                // host answers Denied itself, which is more useful to the
                // viewer than a request that silently disappears.
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++sas_requests_pending;
                    ++sas_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SasResult: {
                if (role != Role::Client) {
                    break;
                }
                SasCode code = SasCode::Failed;
                std::string reason;
                if (!parse_sas_result(payload, code, reason, &error)) {
                    control_protocol_failure("SasResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    sas_code = code;
                    sas_reason = std::move(reason);
                    sas_result_ready = true;
                    ++sas_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::HostActionRequest: {
                if (role != Role::Server) {
                    break;
                }
                HostControlRequest request;
                if (!parse_host_action_request(payload, request, &error)) {
                    control_protocol_failure("HostActionRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // Taps are user-paced; a bounded queue drains stale
                    // toggles instead of growing without limit.
                    if (host_action_requests.size() >= 8) {
                        host_action_requests.pop_front();
                    }
                    host_action_requests.push_back(request);
                    ++host_action_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::ElevationRequest: {
                if (role != Role::Server) break;
                uint32_t request = 0;
                if (!parse_elevation_request(payload, request, &error)) {
                    control_protocol_failure(error); break;
                }
                bool full = false;
                {
                    std::lock_guard lock(mutex);
                    full = elevation_requests.size() >= 8;
                    if (!full) elevation_requests.push_back(request);
                }
                if (full) control_protocol_failure("too many pending elevation requests");
                cv.notify_all();
                break;
            }
            case ControlType::ElevationState: {
                if (role != Role::Client) break;
                ElevationState state;
                if (!parse_elevation_state(payload, state, &error)) {
                    control_protocol_failure(error); break;
                }
                {
                    std::lock_guard lock(mutex);
                    if (elevation_states.size() >= 16) elevation_states.pop_front();
                    elevation_states.push_back(std::move(state));
                }
                cv.notify_all();
                break;
            }
            case ControlType::HostActionResult: {
                if (role != Role::Client) {
                    break;
                }
                HostControlResult result;
                if (!parse_host_action_result(payload, result, &error)) {
                    control_protocol_failure("HostActionResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (host_action_results.size() >= 16) {
                        host_action_results.pop_front();
                    }
                    host_action_results.push_back(std::move(result));
                    ++host_action_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::RemoteConfigRequest: {
                if (role != Role::Server) {
                    break;
                }
                RemoteConfigRequest request;
                if (!parse_remote_config_request(payload, request, &error)) {
                    control_protocol_failure("RemoteConfigRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    remote_config_request = request;
                    remote_config_request_ready = true;
                    ++remote_config_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::RemoteConfigState: {
                if (role != Role::Client) {
                    break;
                }
                RemoteConfigState state;
                if (!parse_remote_config_state(payload, state, &error)) {
                    control_protocol_failure("RemoteConfigState: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    remote_config_state = std::move(state);
                    remote_config_state_ready = true;
                    ++remote_config_states_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplayControl: {
                if (role != Role::Server) {
                    break;
                }
                DisplayControlRequest request;
                if (!parse_display_control(payload, request, &error)) {
                    control_protocol_failure("DisplayControl: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_control_request = request;
                    display_control_request_ready = true;
                    ++display_control_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplayControlState: {
                if (role != Role::Client) {
                    break;
                }
                DisplayControlState state;
                if (!parse_display_control_state(payload, state, &error)) {
                    control_protocol_failure("DisplayControlState: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    display_control_state = std::move(state);
                    display_control_state_ready = true;
                    ++display_control_states_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::OsLoginRequest: {
                if (role != Role::Server) {
                    break;
                }
                std::string password;
                if (!parse_os_login_request(payload, password, &error)) {
                    // The error names the shape, never the password.
                    control_protocol_failure("OsLoginRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (os_login_requests.size() >= 4) {
                        std::string& dropped = os_login_requests.front();
                        std::fill(dropped.begin(), dropped.end(), '\0');
                        os_login_requests.pop_front();
                    }
                    os_login_requests.push_back(std::move(password));
                    ++os_login_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::OsLoginResult: {
                if (role != Role::Client) {
                    break;
                }
                OsLoginCode code = OsLoginCode::Failed;
                std::string reason;
                if (!parse_os_login_result(payload, code, reason, &error)) {
                    control_protocol_failure("OsLoginResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    os_login_code = code;
                    os_login_reason = std::move(reason);
                    os_login_result_ready = true;
                    ++os_login_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SessionListRequest: {
                if (role != Role::Server) {
                    break;
                }
                if (!parse_session_list_request(payload, &error)) {
                    control_protocol_failure("SessionListRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_list_request_pending = true;
                    ++session_list_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SessionListResult: {
                if (role != Role::Client) {
                    break;
                }
                SessionListResult result;
                if (!parse_session_list_result(payload, result, &error)) {
                    control_protocol_failure("SessionListResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_list_result = std::move(result);
                    session_list_result_ready = true;
                    ++session_list_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SessionSelectRequest: {
                if (role != Role::Server) {
                    break;
                }
                uint32_t session_id = 0;
                if (!parse_session_select_request(payload, session_id, &error)) {
                    control_protocol_failure("SessionSelectRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_select_request_id = session_id;
                    session_select_request_ready = true;
                    ++session_select_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SessionSelectResult: {
                if (role != Role::Client) {
                    break;
                }
                SessionSelectResult result;
                if (!parse_session_select_result(payload, result, &error)) {
                    control_protocol_failure("SessionSelectResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_select_result = std::move(result);
                    session_select_result_ready = true;
                    ++session_select_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::SetStreamOptions: {
                if (role != Role::Server) {
                    break;
                }
                // Deliberately not gated on a permission bit: these choices
                // change only what this viewer receives, not the host's
                // desktop, unlike a display switch.
                StreamOptions options;
                if (!parse_set_stream_options(payload, options, &error)) {
                    control_protocol_failure("SetStreamOptions: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stream_options_request = options;
                    stream_options_request_ready = true;
                    ++stream_options_requests_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::StreamOptionsResult: {
                if (role != Role::Client) {
                    break;
                }
                StreamOptionsResult result;
                if (!parse_stream_options_result(payload, result, &error)) {
                    control_protocol_failure("StreamOptionsResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stream_options_result = std::move(result);
                    stream_options_result_ready = true;
                    ++stream_options_results_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::ClipboardUpdate: {
                if (!clipboard_allowed(false)) {
                    // The HelloAck already reported clipboard off; a client
                    // that sends it anyway is dropped, not fatal.
                    break;
                }
                ClipboardUpdate update;
                if (!parse_clipboard_update(payload, update, &error)) {
                    control_protocol_failure("ClipboardUpdate: " + error);
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
            case ControlType::ClipboardFileRead: {
                if (!clipboard_allowed(true)) break;
                ClipboardFileRead request;
                if (!parse_clipboard_file_read(payload, request, &error)) {
                    control_protocol_failure("ClipboardFileRead: " + error); break;
                }
                bool overflow = false;
                {
                    std::lock_guard lock(mutex);
                    if (!clipboard_allowed(true)) break;
                    if (accepted_clipboard_reads.count(request.request_id)) break;
                    overflow = accepted_clipboard_reads.size() >= 8;
                    if (!overflow) {
                        accepted_clipboard_reads.emplace(request.request_id, request);
                        clipboard_file_reads.push_back(request);
                    }
                }
                if (overflow) {
                    // A cancelled old paste can overlap a new one. Refuse the
                    // bounded read rather than terminating a healthy session.
                    (void)send_bytes(control.load(), make_clipboard_file_data(
                        {request.request_id, request.generation}), QUIC_SEND_FLAG_NONE);
                }
                cv.notify_all(); break;
            }
            case ControlType::ClipboardFileData: {
                if (!clipboard_allowed(false)) break;
                ClipboardFileData response;
                if (!parse_clipboard_file_data(payload, response, &error)) {
                    control_protocol_failure("ClipboardFileData: " + error); break;
                }
                {
                    std::lock_guard lock(mutex);
                    if (!clipboard_allowed(false)) break;
                    const auto found = requested_clipboard_reads.find(response.request_id);
                    if (found == requested_clipboard_reads.end() || found->second.generation != response.generation) break;
                    if (response.bytes.size() > found->second.length) { response.ok = false; response.bytes.clear(); }
                    requested_clipboard_reads.erase(found);
                    if (clipboard_file_data.size() < 8) clipboard_file_data.push_back(std::move(response));
                }
                cv.notify_all(); break;
            }
            case ControlType::ChatMessage: {
                std::string text;
                if (!parse_chat_message(payload, text, &error)) {
                    control_protocol_failure("ChatMessage: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (chats.size() >= 64) {
                        break;
                    }
                    chats.push_back(std::move(text));
                    ++chats_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::VoiceCallRequest: {
                VoiceCallRequest request;
                if (!parse_voice_call_request(payload, request, &error)) {
                    control_protocol_failure("VoiceCallRequest: " + error);
                    break;
                }
                if (role == Role::Server) {
                    // A start without the microphone permission is answered
                    // with a refusal rather than queued, so the viewer's call
                    // button never spins. A second start while one is pending
                    // or running is answered Busy for the same reason.
                    VoiceCallResponse refusal;
                    refusal.request_id = request.request_id;
                    bool refuse = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!request.connect) {
                            // A stop cancels this connection's call even if the
                            // start has not been answered yet; drop the queued
                            // start so the host does not open the microphone
                            // for a call the viewer already ended.
                            if (server_voice_pending != 0) {
                                for (auto it = voice_requests.begin();
                                     it != voice_requests.end();) {
                                    if (it->connect &&
                                        it->request_id == server_voice_pending) {
                                        it = voice_requests.erase(it);
                                    } else {
                                        ++it;
                                    }
                                }
                                server_voice_pending = 0;
                            }
                            server_voice_active = false;
                            if (voice_requests.size() >= 4) {
                                voice_requests.pop_front();
                            }
                            voice_requests.push_back(request);
                        } else if ((effective_permissions() & kPermissionMicrophone) ==
                                   0) {
                            refusal.code = VoiceCallCode::Refused;
                            refusal.reason = "microphone access is not granted";
                            refuse = true;
                        } else if (server_voice_pending != 0 || server_voice_active) {
                            refusal.code = VoiceCallCode::Busy;
                            refusal.reason = "a voice call is already running";
                            refuse = true;
                        } else {
                            server_voice_pending = request.request_id;
                            voice_requests.push_back(request);
                        }
                    }
                    if (refuse) {
                        try_send_bytes(control.load(), make_voice_call_response(refusal),
                                       QUIC_SEND_FLAG_NONE);
                    }
                } else {
                    // Hosts only send the stop form; a start from the host is
                    // not this protocol's direction.
                    if (request.connect) {
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        client_voice_active = false;
                        client_voice_requested = false;
                        client_voice_request_id = 0;
                        voice_closed_notice = true;
                        voice_audio.clear();
                        buffered_voice_bytes = 0;
                    }
                }
                cv.notify_all();
                break;
            }
            case ControlType::VoiceCallResponse: {
                if (role != Role::Client) {
                    break;
                }
                VoiceCallResponse response;
                if (!parse_voice_call_response(payload, response, &error)) {
                    control_protocol_failure("VoiceCallResponse: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // An answer for a call the viewer already stopped is
                    // stale, not an error: the stop and the answer cross.
                    if (response.request_id != client_voice_request_id) {
                        break;
                    }
                    client_voice_requested = false;
                    client_voice_request_id = 0;
                    if (response.code == VoiceCallCode::Ok) {
                        client_voice_active = true;
                    } else {
                        client_voice_active = false;
                        voice_audio.clear();
                        buffered_voice_bytes = 0;
                    }
                    while (voice_responses.size() >= 4) {
                        voice_responses.pop_front();
                    }
                    voice_responses.push_back(std::move(response));
                }
                cv.notify_all();
                break;
            }
            case ControlType::VoiceAudio: {
                if (role != Role::Client) {
                    break;
                }
                AudioHeader header;
                const uint8_t* frame = nullptr;
                size_t audio_size = 0;
                if (!parse_voice_audio(payload, header, &frame, audio_size, &error)) {
                    control_protocol_failure("VoiceAudio: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!client_voice_active ||
                        (effective_permissions() & kPermissionMicrophone) == 0) {
                        break;
                    }
                    while (!voice_audio.empty() &&
                           (voice_audio.size() >= kMaxVoiceAudioPackets ||
                            buffered_voice_bytes + payload.size() > kMaxVoiceAudioBytes)) {
                        buffered_voice_bytes -= voice_audio.front().size();
                        voice_audio.pop_front();
                    }
                    if (payload.size() > kMaxVoiceAudioBytes) {
                        break;
                    }
                    voice_audio.push_back(payload);
                    buffered_voice_bytes += payload.size();
                    ++voice_audio_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::ScreenshotRequest: {
                if (role != Role::Server) {
                    break;
                }
                ScreenshotRequest request;
                if (!parse_screenshot_request(payload, request, &error)) {
                    control_protocol_failure("ScreenshotRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // A viewer sends one request at a time; the queue only
                    // absorbs a rapid double click. Past it the newest request
                    // is dropped, because the screen has moved on anyway.
                    if (screenshot_requests.size() < 4) {
                        screenshot_requests.push_back(request);
                    }
                }
                cv.notify_all();
                break;
            }
            case ControlType::ScreenshotResponse: {
                if (role != Role::Client) {
                    break;
                }
                ScreenshotResponse response;
                if (!parse_screenshot_response(payload, response, &error)) {
                    control_protocol_failure("ScreenshotResponse: " + error);
                    break;
                }
                bool mismatched = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (response.request_id != client_screenshot_request_id) {
                        // Slices for a request the viewer abandoned are stale,
                        // not an error: the cancel and the answer cross.
                        break;
                    }
                    if (response.code != ScreenshotCode::Ok) {
                        ScreenshotResult result;
                        result.stream_id = response.stream_id;
                        result.code = response.code;
                        result.reason = std::move(response.reason);
                        client_screenshot_request_id = 0;
                        screenshot_bytes.clear();
                        while (screenshots.size() >= 2) screenshots.pop_front();
                        screenshots.push_back(std::move(result));
                        break;
                    }
                    if (response.index == 0) {
                        screenshot_bytes.clear();
                        screenshot_total = response.total;
                        screenshot_count = response.count;
                        screenshot_next = 0;
                        screenshot_stream = response.stream_id;
                        screenshot_width = response.width;
                        screenshot_height = response.height;
                    } else if (response.index != screenshot_next ||
                               response.total != screenshot_total ||
                               response.count != screenshot_count ||
                               response.stream_id != screenshot_stream ||
                               response.width != screenshot_width ||
                               response.height != screenshot_height) {
                        mismatched = true;
                    }
                    if (!mismatched) {
                        screenshot_bytes.insert(screenshot_bytes.end(), response.data.begin(),
                                                response.data.end());
                        ++screenshot_next;
                        if (screenshot_next == screenshot_count) {
                            if (screenshot_bytes.size() != screenshot_total) {
                                mismatched = true;
                            } else {
                                ScreenshotResult result;
                                result.stream_id = screenshot_stream;
                                result.width = screenshot_width;
                                result.height = screenshot_height;
                                result.png = std::move(screenshot_bytes);
                                client_screenshot_request_id = 0;
                                while (screenshots.size() >= 2) screenshots.pop_front();
                                screenshots.push_back(std::move(result));
                                screenshot_bytes.clear();
                            }
                        }
                    }
                }
                if (mismatched) {
                    control_protocol_failure("ScreenshotResponse: slices do not form one picture");
                }
                cv.notify_all();
                break;
            }
            case ControlType::DisplayCursorUpdate: {
                if (role != Role::Client || !desktop_subscription_ready()) break;
                DisplayCursorUpdate update;
                if (!parse_display_cursor_update(payload, update, &error)) break;
                std::lock_guard<std::mutex> lock(mutex);
                if (subscribed_display_streams.count(update.stream_id)) {
                    coalesce_cursor_update(display_cursor_updates[update.stream_id], std::move(update.cursor));
                    ++cursor_received;
                }
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
                    if (cursor_update) coalesce_cursor_update(*cursor_update, std::move(update));
                    else cursor_update = std::move(update);
                    ++cursor_received;
                }
                break;
            }
            case ControlType::FileOffer: {
                FileOffer offer;
                if (!parse_file_offer(payload, offer, &error)) {
                    control_protocol_failure("FileOffer: " + error);
                    break;
                }
                if (!file_allowed(offer.clipboard, false)) {
                    FileAnswer refused;
                    refused.transfer_id = offer.transfer_id;
                    refused.code = FileCode::Refused;
                    refused.reason = "file or clipboard direction is disabled";
                    try_send_bytes(control.load(), make_file_answer(refused), QUIC_SEND_FLAG_NONE);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // The sender waits for an answer; dropping an offer past
                    // the queue is a refusal by silence only when the queue
                    // never drains, which the receiver prevents.
                    if (file_offers.size() >= kMaxQueuedOffers ||
                        incoming_file_offers.size() >= kMaxQueuedOffers ||
                        incoming_file_offers.count(offer.transfer_id) != 0) {
                        break;
                    }
                    incoming_file_offers.emplace(offer.transfer_id, IncomingFileOffer{offer.clipboard, false});
                    file_offers.push_back(std::move(offer));
                    ++file_offers_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileAnswer: {
                FileAnswer answer;
                if (!parse_file_answer(payload, answer, &error)) {
                    control_protocol_failure("FileAnswer: " + error);
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
                    control_protocol_failure("FileAnswer: resume offset past the file size");
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileConflict: {
                if (role != Role::Client) {
                    break;
                }
                FileConflict conflict;
                if (!parse_file_conflict(payload, conflict, &error)) {
                    control_protocol_failure("FileConflict: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto it = outgoing_uploads.find(conflict.transfer_id);
                    if (it == outgoing_uploads.end() || it->second.finished) {
                        break;
                    }
                    while (file_conflicts.size() >= 8) {
                        file_conflicts.pop_front();
                    }
                    file_conflicts.push_back(std::move(conflict));
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileConflictAnswer: {
                if (role != Role::Server) {
                    break;
                }
                FileConflictAnswer answer;
                if (!parse_file_conflict_answer(payload, answer, &error)) {
                    control_protocol_failure("FileConflictAnswer: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (file_conflict_answers.size() >= 8) {
                        file_conflict_answers.pop_front();
                    }
                    file_conflict_answers.push_back(answer);
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileResult: {
                FileResult result;
                if (!parse_file_result(payload, result, &error)) {
                    control_protocol_failure("FileResult: " + error);
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
                    control_protocol_failure("FileCancel: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    incoming_file_offers.erase(cancel.transfer_id);
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
            case ControlType::FileListRequest: {
                if (role != Role::Server) {
                    break;
                }
                uint32_t request_id = 0;
                std::string directory;
                std::string cursor;
                bool show_hidden = false;
                if (!parse_file_list_request(payload, request_id, directory, &error, &cursor,
                                             &show_hidden)) {
                    control_protocol_failure("FileListRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (file_list_requests.size() >= 8) {
                        file_list_requests.pop_front();
                    }
                    file_list_requests.push_back(
                        {request_id, std::move(directory), std::move(cursor), show_hidden});
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileListResult: {
                if (role != Role::Client) {
                    break;
                }
                FileListResult result;
                if (!parse_file_list_result(payload, result, &error)) {
                    control_protocol_failure("FileListResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (file_list_results.size() >= 8) {
                        file_list_results.pop_front();
                    }
                    file_list_results.push_back(std::move(result));
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileDownloadRequest: {
                if (role != Role::Server) {
                    break;
                }
                uint32_t request_id = 0;
                std::string path;
                if (!parse_file_download_request(payload, request_id, path, &error)) {
                    control_protocol_failure("FileDownloadRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (file_download_requests.size() >= 8) {
                        file_download_requests.pop_front();
                    }
                    file_download_requests.emplace_back(request_id, std::move(path));
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileDownloadResult: {
                if (role != Role::Client) {
                    break;
                }
                FileDownloadResult result;
                if (!parse_file_download_result(payload, result, &error)) {
                    control_protocol_failure("FileDownloadResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (file_download_results.size() >= 8) {
                        file_download_results.pop_front();
                    }
                    file_download_results.push_back(std::move(result));
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileOpRequest: {
                if (role != Role::Server) {
                    break;
                }
                FileOpRequestData request;
                if (!parse_file_op_request(payload, request.request_id, request.op,
                                           request.path, request.new_name, &error)) {
                    control_protocol_failure("FileOpRequest: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (file_op_requests.size() >= 8) {
                        file_op_requests.pop_front();
                    }
                    file_op_requests.push_back(std::move(request));
                }
                cv.notify_all();
                break;
            }
            case ControlType::FileOpResult: {
                if (role != Role::Client) {
                    break;
                }
                FileOpResult result;
                if (!parse_file_op_result(payload, result, &error)) {
                    control_protocol_failure("FileOpResult: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (file_op_results.size() >= 8) {
                        file_op_results.pop_front();
                    }
                    file_op_results.push_back(std::move(result));
                }
                cv.notify_all();
                break;
            }
            case ControlType::PortForwardRequest: {
                if (role != Role::Server) {
                    break;
                }
                PortForwardRequest request;
                if (!parse_port_forward_request(payload, request, &error)) {
                    control_protocol_failure("PortForwardRequest: " + error);
                    break;
                }
                PortForwardResult refusal;
                refusal.forward_id = request.forward_id;
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if ((effective_permissions() & kPermissionPortForward) == 0) {
                        refusal.code = PortForwardCode::Refused;
                        refusal.reason = "port forwarding is disabled on this host";
                    } else if (pending_forward_targets.count(request.forward_id) != 0 ||
                               active_forward_targets.count(request.forward_id) != 0) {
                        refusal.code = PortForwardCode::Busy;
                        refusal.reason = "that forward id is already in use";
                    } else if (pending_forward_targets.size() +
                                       active_forward_targets.size() >=
                                   kMaxPortForwardTargets ||
                               port_forward_requests.size() >= kMaxPortForwardTargets) {
                        refusal.code = PortForwardCode::Busy;
                        refusal.reason = "too many forwarded targets";
                    } else {
                        PortForwardTarget target;
                        target.host = request.host;
                        target.port = request.port;
                        pending_forward_targets.emplace(request.forward_id, std::move(target));
                        port_forward_requests.push_back(std::move(request));
                        queued = true;
                    }
                }
                if (!queued) {
                    // The viewer is waiting for an answer before it dials
                    // locally; a quiet drop would leave its UI spinning.
                    try_send_bytes(control.load(), make_port_forward_result(refusal),
                                   QUIC_SEND_FLAG_NONE);
                }
                cv.notify_all();
                break;
            }
            case ControlType::PortForwardResult: {
                if (role != Role::Client) {
                    break;
                }
                PortForwardResult result;
                if (!parse_port_forward_result(payload, result, &error)) {
                    control_protocol_failure("PortForwardResult: " + error);
                    break;
                }
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto pending = client_pending_forwards.find(result.forward_id);
                    if (pending != client_pending_forwards.end()) {
                        if (result.code == PortForwardCode::Ok) {
                            client_forward_targets[result.forward_id] = pending->second;
                        }
                        client_pending_forwards.erase(pending);
                        port_forward_results.push_back(std::move(result));
                        queued = true;
                    }
                }
                if (queued) {
                    cv.notify_all();
                }
                break;
            }
            case ControlType::PortForwardStop: {
                PortForwardStop stop;
                if (!parse_port_forward_stop(payload, stop, &error)) {
                    control_protocol_failure("PortForwardStop: " + error);
                    break;
                }
                std::vector<HQUIC> aborts;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    aborts = stop_port_forward_target_locked(stop.forward_id);
                    while (port_forward_stops.size() >= kMaxQueuedPortForwardStops) {
                        port_forward_stops.pop_front();
                    }
                    port_forward_stops.push_back(std::move(stop));
                    abort_streams_locked(aborts);
                }
                cv.notify_all();
                break;
            }
            case ControlType::TerminalRequest: {
                if (role != Role::Server) {
                    break;
                }
                TerminalRequest request;
                if (!parse_terminal_request(payload, request, &error)) {
                    control_protocol_failure("TerminalRequest: " + error);
                    break;
                }
                TerminalResult refusal;
                refusal.terminal_id = request.terminal_id;
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if ((effective_permissions() & kPermissionTerminal) == 0) {
                        refusal.code = TerminalCode::Refused;
                        refusal.reason = "terminals are disabled on this host";
                    } else if (pending_terminal_targets.count(request.terminal_id) != 0 ||
                               active_terminal_targets.count(request.terminal_id) != 0) {
                        refusal.code = TerminalCode::Busy;
                        refusal.reason = "that terminal id is already in use";
                    } else if (pending_terminal_targets.size() +
                                       active_terminal_targets.size() >=
                                   kMaxTerminals ||
                               terminal_requests.size() >= kMaxTerminals) {
                        refusal.code = TerminalCode::Busy;
                        refusal.reason = "too many terminals";
                    } else {
                        TerminalSize size;
                        size.terminal_id = request.terminal_id;
                        size.columns = request.columns;
                        size.rows = request.rows;
                        pending_terminal_targets.emplace(request.terminal_id, size);
                        terminal_requests.push_back(std::move(request));
                        queued = true;
                    }
                }
                if (!queued) {
                    // The viewer waits for the answer before it spawns a UI;
                    // a quiet drop would leave it spinning.
                    try_send_bytes(control.load(), make_terminal_result(refusal),
                                   QUIC_SEND_FLAG_NONE);
                }
                cv.notify_all();
                break;
            }
            case ControlType::TerminalResult: {
                if (role != Role::Client) {
                    break;
                }
                TerminalResult result;
                if (!parse_terminal_result(payload, result, &error)) {
                    control_protocol_failure("TerminalResult: " + error);
                    break;
                }
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto pending =
                        client_pending_terminals.find(result.terminal_id);
                    if (pending != client_pending_terminals.end()) {
                        const TerminalSize size = pending->second;
                        client_pending_terminals.erase(pending);
                        if (result.code == TerminalCode::Ok) {
                            // The host may start printing before the viewer
                            // opens its input stream, so the entry exists from
                            // here on and both CTTY streams attach to it.
                            TerminalEntry entry;
                            entry.terminal_id = result.terminal_id;
                            entry.server_side = false;
                            entry.columns = size.columns;
                            entry.rows = size.rows;
                            client_terminal_targets[result.terminal_id] = size;
                            terminal_entries.emplace(result.terminal_id, std::move(entry));
                        }
                        while (terminal_results.size() >= 8) {
                            terminal_results.pop_front();
                        }
                        terminal_results.push_back(std::move(result));
                        queued = true;
                    }
                }
                if (queued) {
                    cv.notify_all();
                }
                break;
            }
            case ControlType::TerminalStop: {
                TerminalStop stop;
                if (!parse_terminal_stop(payload, stop, &error)) {
                    control_protocol_failure("TerminalStop: " + error);
                    break;
                }
                std::vector<HQUIC> aborts;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto found = terminal_entries.find(stop.terminal_id);
                    // The host's stop ends the viewer's input direction, but the
                    // output stream may still hold bytes on the way. Keeping the
                    // entry lets take_terminal_output() hand them over first.
                    // On the host a viewer's stop means the console is closing,
                    // so the terminal ends outright.
                    if (found != terminal_entries.end() && !found->second.server_side) {
                        TerminalEntry& entry = found->second;
                        entry.stopped = true;
                        if (entry.uplink && !entry.send_finished) {
                            aborts.push_back(entry.uplink);
                            entry.uplink = nullptr;
                        }
                        if (entry.receive_finished && entry.received.empty()) {
                            std::vector<HQUIC> dropped =
                                drop_terminal_locked(stop.terminal_id, true);
                            aborts.insert(aborts.end(), dropped.begin(), dropped.end());
                        }
                    } else {
                        std::vector<HQUIC> dropped =
                            drop_terminal_locked(stop.terminal_id, true);
                        aborts.insert(aborts.end(), dropped.begin(), dropped.end());
                    }
                    while (terminal_stops.size() >= 8) {
                        terminal_stops.pop_front();
                    }
                    terminal_stops.push_back(std::move(stop));
                    abort_streams_locked(aborts);
                }
                cv.notify_all();
                break;
            }
            case ControlType::TerminalResize: {
                if (role != Role::Server) {
                    break;
                }
                TerminalSize size;
                if (!parse_terminal_resize(payload, size, &error)) {
                    control_protocol_failure("TerminalResize: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // A resize for a terminal that just ended is stale, not an
                    // error: it can cross the stop on the wire.
                    if (active_terminal_targets.count(size.terminal_id) != 0) {
                        while (terminal_resizes.size() >= 8) {
                            terminal_resizes.pop_front();
                        }
                        terminal_resizes.push_back(size);
                    }
                }
                cv.notify_all();
                break;
            }
            case ControlType::ListCameras: {
                if (role != Role::Server) {
                    break;
                }
                if (!parse_camera_list_request(payload, &error)) {
                    control_protocol_failure("ListCameras: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if ((effective_permissions() & kPermissionCamera) != 0) {
                        camera_list_request_ready = true;
                    }
                }
                cv.notify_all();
                break;
            }
            case ControlType::CameraList: {
                if (role != Role::Client) {
                    break;
                }
                CameraList list;
                if (!parse_camera_list(payload, list, &error)) {
                    control_protocol_failure("CameraList: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // The answer is a snapshot: a slow host loop wants the
                    // newest list, not a replay.
                    while (camera_lists_queue.size() >= 4) {
                        camera_lists_queue.pop_front();
                    }
                    camera_lists_queue.push_back(std::move(list));
                    ++camera_lists_seen;
                }
                cv.notify_all();
                break;
            }
            case ControlType::StartCamera: {
                if (role != Role::Server) {
                    break;
                }
                uint16_t camera_id = 0;
                if (!parse_camera_start(payload, camera_id, &error)) {
                    control_protocol_failure("StartCamera: " + error);
                    break;
                }
                CameraResult refusal;
                refusal.camera_id = camera_id;
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if ((effective_permissions() & kPermissionCamera) == 0) {
                        refusal.code = CameraCode::Refused;
                        refusal.reason = "cameras are disabled on this host";
                    } else if ((camera.load() != nullptr && !camera_finished) ||
                               server_pending_camera != 0) {
                        refusal.code = CameraCode::Busy;
                        refusal.reason = "another camera is already streaming";
                    } else if (camera_starts.size() >= 4) {
                        refusal.code = CameraCode::Busy;
                        refusal.reason = "too many camera requests";
                    } else if (std::find(camera_starts.begin(), camera_starts.end(),
                                         camera_id) != camera_starts.end()) {
                        refusal.code = CameraCode::Busy;
                        refusal.reason = "that camera is already requested";
                    } else {
                        camera_starts.push_back(camera_id);
                        server_pending_camera = camera_id;
                        queued = true;
                    }
                }
                if (!queued) {
                    // The viewer waits for the answer before it shows a
                    // picture; a quiet drop would leave its panel spinning.
                    try_send_bytes(control.load(), make_camera_result(refusal),
                                   QUIC_SEND_FLAG_NONE);
                }
                cv.notify_all();
                break;
            }
            case ControlType::CameraResult: {
                if (role != Role::Client) {
                    break;
                }
                CameraResult result;
                if (!parse_camera_result(payload, result, &error)) {
                    control_protocol_failure("CameraResult: " + error);
                    break;
                }
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // The CCAM stream may already have arrived and cleared the
                    // pending id; the running id accepts that late answer.
                    if (result.camera_id == client_pending_camera ||
                        result.camera_id == client_camera_id) {
                        client_pending_camera = 0;
                        if (result.code == CameraCode::Ok) {
                            client_camera_id = result.camera_id;
                        } else if (result.camera_id == client_camera_id) {
                            client_camera_id = 0;
                            camera_finished = true;
                        }
                        while (camera_results.size() >= 8) {
                            camera_results.pop_front();
                        }
                        camera_results.push_back(std::move(result));
                        queued = true;
                    }
                }
                if (queued) {
                    cv.notify_all();
                }
                break;
            }
            case ControlType::StopCamera: {
                if (role != Role::Server) {
                    break;
                }
                uint16_t camera_id = 0;
                if (!parse_camera_stop(payload, camera_id, &error)) {
                    control_protocol_failure("StopCamera: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (camera_stops.size() >= 4) {
                        camera_stops.pop_front();
                    }
                    camera_stops.push_back(camera_id);
                    if (camera_id == server_pending_camera) {
                        // The viewer gave up while the host was still opening
                        // the device; a later send_camera_result is refused.
                        server_pending_camera = 0;
                    }
                    // A stale stop for an earlier camera must not end the one
                    // that replaced it; the id check is the guard.
                    if (camera_id == server_camera_id && !camera_finished) {
                        camera_finished = true;
                        server_camera_id = 0;
                        // The host application still pops the id and stops its
                        // capture; the FIN tells the viewer no more frames
                        // follow. The shutdown runs under the lock because the
                        // handle is released by SHUTDOWN_COMPLETE, which takes
                        // the same lock.
                        if (HQUIC stream = camera.load()) {
                            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
                        }
                    }
                }
                cv.notify_all();
                break;
            }
            default:
                break;
        }
    }

    // ----- sending -----

    void release_send(size_t bytes, uint64_t generation, bool screenshot = false) {
        {
            std::lock_guard<std::mutex> lock(send_mutex);
            // The queue was reset for a new connection: this completion
            // belongs to the old one and its counters are already gone.
            if (generation != send_generation) {
                return;
            }
            --in_flight_sends;
            in_flight_bytes -= bytes;
            if (screenshot) screenshot_in_flight_bytes -= bytes;
        }
        send_cv.notify_all();
    }

    // Teardown drops sends that will never complete. Bumping the generation
    // makes late completions from the old connection no-ops instead of
    // underflowing the counters of the session that replaces it.
    void reset_send_queue() {
        {
            std::lock_guard<std::mutex> lock(send_mutex);
            ++send_generation;
            in_flight_bytes = 0;
            screenshot_in_flight_bytes = 0;
            in_flight_sends = 0;
        }
        send_cv.notify_all();
    }

    // v35: one whole audio packet as a QUIC DATAGRAM. Never waits. When the
    // congestion window is full the frame is lost and playback conceals it;
    // queueing it would recreate the reliable stream's head-of-line stall.
    Error send_audio_datagram(std::vector<uint8_t> bytes) {
        HQUIC handle = connection.load();
        if (!handle || transport_down.load()) {
            return Error(Status::Unavailable, "the connection closed before the send");
        }
        const size_t limit = datagram_max_send_length.load();
        if (limit == 0 || bytes.size() > limit) {
            // One frame that does not fit the path is a 20 ms gap, not a
            // reason to stop audio; the announcement already required room
            // for a full packet when the mode was latched.
            return Error(Status::WaitTimeout, "the audio packet does not fit a datagram");
        }
        auto* send = new DatagramSend;
        send->bytes = std::move(bytes);
        send->buffer.Buffer = send->bytes.data();
        send->buffer.Length = static_cast<uint32_t>(send->bytes.size());
        {
            std::lock_guard<std::mutex> lock(datagram_mutex);
            datagram_sends.push_back(send);
        }
        const QUIC_STATUS status =
            api->DatagramSend(handle, &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
        if (QUIC_FAILED(status)) {
            {
                std::lock_guard<std::mutex> lock(datagram_mutex);
                datagram_sends.erase(
                    std::remove(datagram_sends.begin(), datagram_sends.end(), send),
                    datagram_sends.end());
            }
            delete send;
            if (status == QUIC_STATUS_ABORTED || status == QUIC_STATUS_INVALID_STATE) {
                return Error(Status::Unavailable, "the connection closed before the send");
            }
            if (status == QUIC_STATUS_BUFFER_TOO_SMALL ||
                status == QUIC_STATUS_OUT_OF_MEMORY) {
                return Error(Status::WaitTimeout, "the datagram send queue is full");
            }
            return quic_error(status, "QUIC DatagramSend");
        }
        return Error::success();
    }

    // Callback replies and real-time media never wait for transport capacity.
    Error send_bytes_impl(HQUIC stream, std::vector<uint8_t> bytes, QUIC_SEND_FLAGS flags,
                          bool wait_for_capacity,
                          std::chrono::steady_clock::time_point deadline = kNoSendDeadline,
                          bool screenshot = false) {
        if (!stream) {
            return Error(Status::Unavailable, "no stream is open");
        }
        if (bytes.empty()) {
            return Error(Status::InvalidCall, "refusing to send an empty message");
        }
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(send_mutex);
            const auto has_capacity = [this] {
                return (in_flight_sends < kMaxInFlightSends &&
                        in_flight_bytes < kMaxInFlightBytes) ||
                       transport_down.load();
            };
            if (wait_for_capacity) {
                if (deadline == kNoSendDeadline) {
                    if (!send_cv.wait_for(lock, kSendCapacityTimeout, has_capacity)) {
                        return Error(Status::WaitTimeout, "send queue did not drain");
                    }
                } else if (!send_cv.wait_until(lock, deadline, has_capacity)) {
                    return Error(Status::WaitTimeout, "send queue is full");
                }
            } else if (!has_capacity()) {
                return Error(Status::WaitTimeout, "send queue is full");
            }
            if (transport_down.load()) {
                return Error(Status::Unavailable, "connection closed before send");
            }
            if (screenshot && screenshot_in_flight_bytes + bytes.size() > 512u * 1024u)
                return Error(Status::WaitTimeout, "screenshot send window is full");
            ++in_flight_sends;
            if (screenshot) screenshot_in_flight_bytes += bytes.size();
            in_flight_bytes += bytes.size();
            generation = send_generation;
        }

        auto* pending = new PendingSend;
        pending->bytes = std::move(bytes);
        pending->generation = generation;
        pending->screenshot = screenshot;
        pending->buffer.Buffer = pending->bytes.data();
        pending->buffer.Length = static_cast<uint32_t>(pending->bytes.size());
        const QUIC_STATUS status =
            api->StreamSend(stream, &pending->buffer, 1, flags, pending);
        if (QUIC_FAILED(status)) {
            release_send(pending->bytes.size(), pending->generation, pending->screenshot);
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

    // Caller holds mutex. A single large IDR is allowed into an empty window;
    // subsequent frames wait for the application to consume it.
    bool video_capacity_locked(size_t bytes = 0) const {
        return pending_video.empty() ||
            (pending_video.size() < kMaxPendingVideoFrames &&
             pending_video_bytes + bytes <= kMaxPendingVideoBytes &&
             steady_us() - pending_video.front().sent_us < kVideoDeliveryWindowUs);
    }

    void flush_video_receipt() {
        std::optional<uint64_t> sequence;
        {
            std::lock_guard<std::mutex> lock(mutex);
            sequence = pending_video_receipt;
        }
        if (!sequence) return;
        auto sent = try_send_bytes(control.load(), make_video_receipt(*sequence), QUIC_SEND_FLAG_NONE);
        if (sent) {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending_video_receipt == sequence) pending_video_receipt.reset();
        }
    }

    // A forwarded TCP pump may not block forever: a stalled queue returns
    // WaitTimeout with nothing sent, and the caller retries with its buffer.
    Error send_bytes_until(HQUIC stream, std::vector<uint8_t> bytes, QUIC_SEND_FLAGS flags,
                           std::chrono::steady_clock::time_point deadline) {
        return send_bytes_impl(stream, std::move(bytes), flags, true, deadline);
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
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(send_mutex);
            const auto has_capacity = [this] {
                return (in_flight_sends < kMaxInFlightSends &&
                        in_flight_bytes < kMaxInFlightBytes) ||
                       transport_down.load();
            };
            if (!send_cv.wait_for(lock, kSendCapacityTimeout, has_capacity)) {
                return Error(Status::WaitTimeout, "send queue did not drain");
            }
            if (transport_down.load()) {
                return Error(Status::Unavailable, "connection closed before send");
            }
            ++in_flight_sends;
            generation = send_generation;
        }
        auto* pending = new PendingSend;
        pending->generation = generation;
        static uint8_t empty = 0;
        pending->buffer.Buffer = &empty;
        pending->buffer.Length = 0;
        const QUIC_STATUS status =
            api->StreamSend(stream, &pending->buffer, 1, QUIC_SEND_FLAG_FIN, pending);
        if (QUIC_FAILED(status)) {
            release_send(0, pending->generation);
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
        send_cv.wait_for(lock, timeout, [this] {
            return in_flight_sends == 0 || transport_down.load();
        });
        // Waking because the transport went down is not a drain; the caller
        // needs to know whether the queue is actually empty.
        return in_flight_sends == 0;
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
        if (!wait_sends(std::chrono::milliseconds(500))) {
            reset_send_queue();
        }
        if (HQUIC handle = audio.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (HQUIC handle = video.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (HQUIC handle = control.exchange(nullptr)) {
            api->StreamClose(handle);
        }
        if (HQUIC handle = camera.exchange(nullptr)) {
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
        // Datagram sends die with the connection: their final send state never
        // arrives once ConnectionClose returned, so release the buffers here.
        {
            std::lock_guard<std::mutex> lock(datagram_mutex);
            for (DatagramSend* send : datagram_sends) {
                delete send;
            }
            datagram_sends.clear();
        }
        datagram_send_enabled.store(false);
        datagram_max_send_length.store(0);
        client_audio_datagram.store(false);
        audio_datagram_active.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = false;
            connected_at = {};
            client_accepted = false;
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
            host_nonce = {};
            host_nonce_sent = false;
            auth_challenge_ready = false;
            ticket_renew_ready = false;
            ticket_renew = TicketRenewRequest{};
            ticket_renew_result_ready = false;
            ticket_renew_result = TicketRenewResult{};
            client_nonce = {};
            initial_codec_preference = StreamCodecPreference::Auto;
            requested_display = kDisplayAuto;
            decoding_codecs = kVideoCodecAll;
            decoding_444 = 0;
            initial_chroma_preference = VideoChroma::Yuv420;
            requested_mode = SessionMode::Desktop;
            requested_peer.clear();
            session_audit_ref.clear();
            display_switch_ready = false;
            display_switch_index = kDisplayAuto;
            display_switch_result_ready = false;
            display_switch_result_index = 0;
            display_switch_code = DisplaySwitchCode::Ok;
            display_switch_reason.clear();
            display_switch_results_seen = 0;
            capture_display_requests.clear();
            capture_display_results.clear();
            subscribed_display_streams.clear();
            display_cursor_updates.clear();
            display_stream_updates.clear();
            display_keyframe_requests.clear();
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
            privacy_mode_request_ready = false;
            privacy_mode_requested = false;
            privacy_mode_requests_seen = 0;
            privacy_mode_state_ready = false;
            privacy_mode_enabled = false;
            privacy_mode_code = PrivacyModeCode::Ok;
            privacy_mode_reason.clear();
            privacy_mode_states_seen = 0;
            desktop_state_ready = false;
            desktop_kind = DesktopKind::Default;
            desktop_states_seen = 0;
            sas_requests_pending = 0;
            sas_requests_seen = 0;
            sas_result_ready = false;
            sas_code = SasCode::Ok;
            sas_reason.clear();
            sas_results_seen = 0;
            host_action_requests.clear();
            elevation_requests.clear();
            elevation_states.clear();
            host_action_requests_seen = 0;
            host_action_results.clear();
            host_action_results_seen = 0;
            remote_config_request_ready = false;
            remote_config_request = RemoteConfigRequest{};
            remote_config_requests_seen = 0;
            remote_config_state_ready = false;
            remote_config_state = RemoteConfigState{};
            remote_config_states_seen = 0;
            display_control_request_ready = false;
            display_control_request = DisplayControlRequest{};
            display_control_requests_seen = 0;
            display_control_state_ready = false;
            display_control_state = DisplayControlState{};
            display_control_states_seen = 0;
            for (std::string& password : os_login_requests) {
                std::fill(password.begin(), password.end(), '\0');
            }
            os_login_requests.clear();
            os_login_requests_seen = 0;
            os_login_result_ready = false;
            os_login_code = OsLoginCode::Ok;
            os_login_reason.clear();
            os_login_results_seen = 0;
            session_list_request_pending = false;
            session_list_requests_seen = 0;
            session_select_request_ready = false;
            session_select_request_id = 0;
            session_select_requests_seen = 0;
            session_list_result_ready = false;
            session_list_result = SessionListResult{};
            session_list_results_seen = 0;
            session_select_result_ready = false;
            session_select_result = SessionSelectResult{};
            session_select_results_seen = 0;
            stream_options_request_ready = false;
            stream_options_request = StreamOptions{};
            stream_options_requests_seen = 0;
            stream_options_result_ready = false;
            stream_options_result = StreamOptionsResult{};
            stream_options_results_seen = 0;
            session_permissions = 0;
            pending_policy_revocations = 0;
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
            cursor_hint_ready = false;
            cursor_hint = ShowCursorHint{};
            cursor_hints_seen = 0;
            clipboard_updates.clear();
            clipboard_received = 0;
            cursor_update.reset();
            cursor_received = 0;
            latency = LatencySample{};
            last_reply_probe_us = 0;
            pending_qos_probe_us = 0;
            last_qos_probe_send_us = 0;
            pending_video.clear();
            pending_video_bytes = 0;
            pending_video_receipt.reset();
            received.clear();
            audio_received.clear();
            audio_datagrams.clear();
            audio_datagram_drain_until = std::chrono::steady_clock::time_point{};
            control_parser.reset();
            frame_parser.reset();
            audio_parser.reset();
            microphone_streams_seen = 0;
            camera_finished = false;
            server_camera_id = 0;
            server_pending_camera = 0;
            client_pending_camera = 0;
            client_camera_id = 0;
            camera_list_request_ready = false;
            camera_lists_queue.clear();
            camera_lists_seen = 0;
            camera_starts.clear();
            camera_results.clear();
            camera_stops.clear();
            camera_received.clear();
            camera_parser.reset();
            server_voice_active = false;
            client_voice_active = false;
            client_voice_requested = false;
            client_voice_request_id = 0;
            server_voice_pending = 0;
            next_voice_request_id = 0;
            next_voice_seq = 0;
            voice_closed_notice = false;
            voice_audio_seen = 0;
            voice_requests.clear();
            voice_responses.clear();
            voice_audio.clear();
            buffered_voice_bytes = 0;
            screenshot_requests.clear();
            next_screenshot_request_id = 0;
            client_screenshot_request_id = 0;
            screenshot_total = 0;
            screenshot_count = 0;
            screenshot_next = 0;
            screenshot_stream = 0;
            screenshot_width = 0;
            screenshot_height = 0;
            screenshot_bytes.clear();
            screenshots.clear();
            unclassified.clear();
            transport_failed = false;
            transport_status = Status::Ok;
            transport_message.clear();
        }
        transport_down = false;
        next_audio_seq = 0;
        next_microphone_seq = 0;
        next_camera_seq = 0;
        // Only now can the listener claim the slot for the next client.
        connection = nullptr;
        cv.notify_all();
        send_cv.notify_all();
    }

    // The hard gates from QuicServerConfig. Authentication and approval may
    // only reduce this; the transport never grants beyond it.
    PermissionMask config_permissions() const {
        if (server_config.permission_policy) {
            return server_config.permission_policy->load() & kPermissionAll;
        }
        PermissionMask mask = 0;
        if (server_config.allow_input) {
            mask |= kPermissionInput;
        }
        if (server_config.allow_clipboard && server_config.clipboard_direction != ClipboardDirection::Disabled) {
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
        if (server_config.allow_privacy_mode) {
            mask |= kPermissionPrivacyMode;
        }
        if (server_config.allow_port_forward) {
            mask |= kPermissionPortForward;
        }
        if (server_config.allow_terminal) {
            mask |= kPermissionTerminal;
        }
        if (server_config.allow_camera) {
            mask |= kPermissionCamera;
        }
        if (server_config.allow_record) {
            mask |= kPermissionRecord;
        }
        if (server_config.allow_restart) {
            mask |= kPermissionRestart;
        }
        if (server_config.allow_block_input) {
            mask |= kPermissionBlockInput;
        }
        if (server_config.allow_os_login) {
            mask |= kPermissionOsLogin;
        }
        if (server_config.allow_session_select) {
            mask |= kPermissionSessionSelect;
        }
        if (server_config.allow_remote_config) {
            mask |= kPermissionRemoteConfig;
        }
        return mask;
    }

    PermissionMask effective_permissions() const {
        const PermissionMask granted = session_permissions.load();
        return role == Role::Server ? granted & config_permissions() : granted;
    }

    bool clipboard_allowed(bool sending) const {
        const auto direction = role == Role::Server ? server_config.clipboard_direction
                                                     : peer_clipboard_direction.load();
        return (effective_permissions() & kPermissionClipboard) != 0 &&
            ((role == Role::Server) == sending ? clipboard_from_host(direction)
                                               : clipboard_to_host(direction));
    }
    bool file_allowed(bool clipboard, bool sending) const {
        return clipboard ? clipboard_allowed(sending)
                         : (effective_permissions() & kPermissionFileTransfer) != 0;
    }

    // Caller holds mutex; clipboard files and browser transfers have
    // independent grants. Remove queued bytes before aborting their streams.
    void revoke_file_permissions_locked() {
        std::erase_if(file_offers, [&](const auto& offer) { return !file_allowed(offer.clipboard, false); });
        std::erase_if(incoming_file_offers, [&](const auto& item) { return !file_allowed(item.second.clipboard, false); });
        for (auto& [id, upload] : received_uploads) {
            if (!file_allowed(upload.clipboard, false)) {
                upload.finished = true;
                upload.error = "file or clipboard permission was disabled";
                drop_file_chunks_locked(id);
                if (upload.stream) api->StreamShutdown(upload.stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            }
        }
        for (auto& [id, upload] : outgoing_uploads) {
            if (!file_allowed(upload.offer.clipboard, true)) {
                upload.finished = true;
                if (upload.stream) api->StreamShutdown(upload.stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            }
        }
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
        // Wake accept() and give queued viewers back to the transport. Their
        // destructors close the connections below, outside the accept lock.
        std::deque<std::unique_ptr<QuicSession>> pending;
        {
            std::lock_guard<std::mutex> lock(accept_mutex);
            listener_closed = true;
            pending.swap(accepted_sessions);
        }
        accept_cv.notify_all();
        pending.clear();
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
        if (!wait_sends(std::chrono::milliseconds(1000))) {
            reset_send_queue();
        }

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
        if (HQUIC handle = camera.exchange(nullptr)) {
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
        {
            std::lock_guard<std::mutex> lock(datagram_mutex);
            for (DatagramSend* send : datagram_sends) {
                delete send;
            }
            datagram_sends.clear();
        }
        if (configuration && owns_transport) {
            api->ConfigurationClose(configuration);
            configuration = nullptr;
        }
        if (registration && owns_transport) {
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
    if (config.max_clients > 1) {
        impl.live_clients = std::make_shared<std::atomic<size_t>>(0);
        if (config.max_authenticated_clients)
            impl.authenticated_clients = std::make_shared<std::atomic<size_t>>(0);
    }

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
    settings.SendBufferingEnabled = FALSE;
    settings.IsSet.SendBufferingEnabled = TRUE;
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
        kMaxClipboardStreams + kMaxIncomingUploads + kMaxPortForwardConnections + 2);
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    settings.IsSet.ServerResumptionLevel = TRUE;
    // v35: receive datagrams so the transport advertises a datagram frame
    // size; system audio leaves the host on this path.
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;

    QUIC_BUFFER alpn{};
    alpn.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(kAlpn));
    alpn.Length = static_cast<uint32_t>(std::strlen(kAlpn));
    status = impl.api->ConfigurationOpen(impl.registration, &alpn, 1, &settings,
                                         sizeof(settings), nullptr, &impl.configuration);
    if (QUIC_FAILED(status)) {
        *error = "QUIC ConfigurationOpen failed: " + status_hex(status);
        return nullptr;
    }

    const auto load_identity = [&]() {
        ScopedUserContext original(config.identity_user_token.get());
        if (!original.ok()) {
            *error = "cannot enter the original host identity: " + std::to_string(original.error());
            return false;
        }
        impl.certificate = open_or_create_identity(config.identity_scope, config.expected_identity_fingerprint, error);
        if (!impl.certificate) {
            return false;
        }
        if (config.identity_user_token) {
            // SChannel can use the certificate after this thread returns to
            // its elevated identity. Cache the original user's acquired key
            // on the certificate instead of reopening its container later.
            HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
            DWORD spec = 0;
            BOOL caller_frees = FALSE;
            if (!CryptAcquireCertificatePrivateKey(impl.certificate,
                    CRYPT_ACQUIRE_CACHE_FLAG | CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
                    nullptr, &key, &spec, &caller_frees)) {
                *error = "cannot acquire the original user's identity key: " + std::to_string(GetLastError());
                return false;
            }
            if (caller_frees) {
                if (spec == CERT_NCRYPT_KEY_SPEC) NCryptFreeObject(key);
                else CryptReleaseContext(key, 0);
                *error = "the original identity key could not be cached";
                return false;
            }
        }
        impl.identity_fingerprint = certificate_fingerprint(impl.certificate);
        if (impl.identity_fingerprint.empty()) {
            *error = "cannot fingerprint the server identity certificate";
            return false;
        }
        QUIC_CREDENTIAL_CONFIG credential{};
        credential.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
        credential.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        credential.CertificateContext =
            reinterpret_cast<QUIC_CERTIFICATE*>(const_cast<CERT_CONTEXT*>(impl.certificate));
        status = impl.api->ConfigurationLoadCredential(impl.configuration, &credential);
        if (QUIC_FAILED(status)) {
            *error = "QUIC ConfigurationLoadCredential failed: " + status_hex(status);
            return false;
        }
        return true;
    };
    if (!load_identity()) return nullptr;

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
    if (config.allow_input && config.max_clients <= 1) {
        impl.injector = InputInjector::create(config.dry_run_input, error);
        if (!impl.injector) {
            return nullptr;
        }
    }
    return session;
}

Error QuicSession::accept(std::unique_ptr<QuicSession>& client,
                          std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server || impl.server_config.max_clients <= 1) {
        return Error(Status::InvalidCall,
                     "accept needs a server created with max_clients above 1");
    }
    std::unique_lock<std::mutex> lock(impl.accept_mutex);
    const bool ready = impl.accept_cv.wait_for(lock, timeout, [&impl] {
        return !impl.accepted_sessions.empty() || impl.listener_closed;
    });
    if (!impl.accepted_sessions.empty()) {
        client = std::move(impl.accepted_sessions.front());
        impl.accepted_sessions.pop_front();
        lock.unlock();
        if (client->impl_->server_config.allow_input) {
            std::string error;
            client->impl_->injector = InputInjector::create(
                client->impl_->server_config.dry_run_input, &error);
            if (!client->impl_->injector) {
                client.reset();
                return Error(Status::Unavailable, "input injector: " + error);
            }
        }
        return Error::success();
    }
    if (!ready) {
        return Error(Status::WaitTimeout, "no viewer connected in time");
    }
    return Error(Status::Unavailable, "the listener is closed");
}

void QuicSession::set_server_callbacks(
    std::function<ClientAuthResult(const ClientRequest&)> authenticate,
    std::function<ClientApproval(const ClientRequest&)> approve_client) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->server_config.authenticate = std::move(authenticate);
    impl_->server_config.approve_client = std::move(approve_client);
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
    settings.SendBufferingEnabled = FALSE;
    settings.IsSet.SendBufferingEnabled = TRUE;
    settings.IdleTimeoutMs = config.idle_timeout_ms;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.KeepAliveIntervalMs = kKeepAliveMs;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    // The server opens one unidirectional video stream and, when it shares
    // system audio, a second for audio. Its rich clipboard blobs use more, so
    // the cap is the receive-side stream budget rather than two.
    settings.PeerUnidiStreamCount =
        static_cast<uint16_t>(kMaxClipboardStreams + kMaxPortForwardConnections + 2);
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    // v35: receiving datagrams is what tells the host its datagram audio is
    // welcome; a client that leaves this off gets the reliable stream.
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;

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
    const PermissionMask ceiling = impl.config_permissions();
    for (;;) {
        std::unique_lock<std::mutex> lock(impl.mutex);
        auto until = deadline;
        if (!impl.client_accepted && !impl.hello_ready && impl.connected &&
            impl.server_config.hello_timeout_ms > 0) {
            const auto hello_deadline =
                impl.connected_at +
                std::chrono::milliseconds(impl.server_config.hello_timeout_ms);
            if (std::chrono::steady_clock::now() >= hello_deadline) {
                lock.unlock();
                // A peer that connected but never sent its Hello must not hold
                // the single session slot forever (the keepalive defeats the
                // idle timeout). Close it and keep listening for a real client.
                impl.reset_connection();
                if (std::chrono::steady_clock::now() >= deadline) {
                    return Error(Status::WaitTimeout, "client connected but sent no Hello");
                }
                continue;
            }
            if (hello_deadline < until) {
                until = hello_deadline;
            }
        }
        // A notification delivered while this thread was refusing or
        // resetting the previous client is not repeated, so a state left
        // pending by it (the next viewer's Hello) must be seen here rather
        // than slept through until the deadline. CONNECTED still wakes the
        // wait so the pre-Hello deadline is re-evaluated without a Hello.
        if (!impl.hello_ready && !impl.transport_failed && !impl.peer_closed &&
            !impl.shutdown_complete) {
            impl.cv.wait_until(lock, until);
        }
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.hello_ready) {
            // Consumed here: the next wait must be triggered by a new Hello,
            // not by the one this call already answered.
            impl.hello_ready = false;
            ClientRequest request;
            request.display = impl.requested_display;
            request.mode = impl.requested_mode;
            request.peer_address = impl.requested_peer;
            request.hello = std::move(impl.pending_hello);
            request.host_nonce = impl.host_nonce;
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
                } else if (!impl.peer_alive()) {
                    // The Hello can reach this loop after the viewer's
                    // handshake timeout expired. Do not ask an operator about
                    // a viewer that already gave up.
                    refuse(AccessDenial::AuthTimeout,
                           "the viewer stopped waiting for approval");
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

            if (refusal.empty() && impl.authenticated_clients && !impl.authenticated_slot) {
                const auto count = impl.authenticated_clients->fetch_add(1) + 1;
                if (count > impl.server_config.max_authenticated_clients) {
                    impl.authenticated_clients->fetch_sub(1);
                    refuse(AccessDenial::Declined, "the host's viewer limit has been reached");
                } else impl.authenticated_slot = true;
            }
            if (refusal.empty()) {
                if (!impl.server_config.authenticate) {
                    impl.consume_token();
                }
                {
                    std::lock_guard<std::mutex> guard(impl.mutex);
                    // Approval may have waited while another session reduced
                    // policy. Reopening it must not revive that pending grant.
                    const PermissionMask granted = auth.permissions & ceiling &
                        impl.config_permissions() & ~impl.pending_policy_revocations;
                    impl.session_permissions = granted;
                    impl.permission_ceiling = granted;
                    impl.client_accepted = true;
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
        if (impl.peer_closed || impl.shutdown_complete) {
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
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(Status::WaitTimeout, "no client connected in time");
        }
        // A spurious wakeup or the end of the pre-Hello slice: the next
        // iteration either re-waits or enforces the pre-Hello timeout.
        continue;
    }
}

std::optional<AccessDenial> QuicSession::denial() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->denial_code;
}

Error QuicSession::renew_access_ticket(const std::vector<uint8_t>& signed_ticket,
                                       std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "renew_access_ticket is a client operation");
    }
    if (signed_ticket.size() <= kTicketSignatureSize ||
        signed_ticket.size() > kTicketMaxSigned) {
        return Error(Status::InvalidCall, "the renewal ticket is malformed");
    }
    if (impl.client_config.access_ticket_key.size() != kTicketClientKeySize) {
        return Error(Status::InvalidCall,
                     "the renewal needs the session's 32-byte client key");
    }
    AccessTicket ticket;
    // decode_access_ticket takes the envelope; the signature stays attached
    // because the proof covers the signed bytes the host will verify.
    const std::vector<uint8_t> envelope(signed_ticket.begin(),
                                        signed_ticket.end() - kTicketSignatureSize);
    if (decode_access_ticket(envelope, &ticket) != TicketError::None) {
        return Error(Status::InvalidCall, "the renewal ticket is malformed");
    }
    std::array<uint8_t, kTicketProofHostNonceSize> host_nonce{};
    std::array<uint8_t, kHelloNonceBytes> client_nonce{};
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "handshake not complete");
        }
        if (impl.client_config.access_ticket.empty()) {
            return Error(Status::InvalidCall,
                         "this session was not admitted by an access ticket");
        }
        host_nonce = impl.host_nonce;
        client_nonce = impl.client_nonce;
        impl.ticket_renew_result_ready = false;
    }
    const std::vector<uint8_t> message = access_ticket_proof_message(
        ticket.host_id, ticket.host_public_key_fingerprint, host_nonce,
        std::vector<uint8_t>(client_nonce.begin(), client_nonce.end()), signed_ticket);
    std::vector<uint8_t> proof;
    if (!sign_access_ticket_proof(impl.client_config.access_ticket_key, message, &proof)) {
        return Error(Status::CaptureFailed, "cannot sign the renewal proof");
    }
    const Error sent = impl.send_bytes(impl.control.load(),
                                       make_ticket_renew(signed_ticket, proof),
                                       QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        return sent;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    impl.cv.wait_until(lock, deadline, [&impl] {
        return impl.ticket_renew_result_ready || impl.transport_failed ||
               impl.shutdown_complete || impl.peer_closed;
    });
    if (impl.ticket_renew_result_ready) {
        const TicketRenewResult result = impl.ticket_renew_result;
        impl.ticket_renew_result_ready = false;
        if (result.code != TicketRenewCode::Accepted) {
            return Error(Status::PermissionDenied,
                         result.reason.empty() ? "the host refused the ticket renewal"
                                               : result.reason);
        }
        // The next renewal signs over this ticket, so keep it as the current
        // one even if the caller drops its own copy.
        impl.client_config.access_ticket = signed_ticket;
        return Error::success();
    }
    if (impl.transport_failed) {
        return Error(impl.transport_status, impl.transport_message);
    }
    if (impl.peer_closed || impl.shutdown_complete) {
        return Error(Status::Unavailable, "the host closed the session during the renewal");
    }
    return Error(Status::WaitTimeout, "the host did not answer the ticket renewal");
}

uint16_t QuicSession::requested_display() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->requested_display;
}

VideoCodecMask QuicSession::decoding_codecs() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->decoding_codecs;
}

StreamCodecPreference QuicSession::codec_preference() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initial_codec_preference;
}
VideoCodecMask QuicSession::decoding_444() const {
    std::lock_guard lock(impl_->mutex); return impl_->decoding_444;
}
VideoChroma QuicSession::chroma_preference() const {
    std::lock_guard lock(impl_->mutex); return impl_->initial_chroma_preference;
}

SessionMode QuicSession::requested_mode() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->requested_mode;
}

bool QuicSession::peer_alive() const {
    return impl_->peer_alive();
}

bool QuicSession::viewer_finished() const {
    Impl& impl = *impl_;
    if (impl.transport_down.load()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.stats_ready || impl.bye_ready || impl.peer_closed || impl.transport_failed ||
           impl.shutdown_complete;
}

std::string QuicSession::audit_ref() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session_audit_ref;
}

std::string QuicSession::server_fingerprint() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->identity_fingerprint;
}

std::string QuicSession::client_address() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->requested_peer;
}

PermissionMask QuicSession::permissions() const {
    return impl_->effective_permissions();
}

Error QuicSession::set_permissions(PermissionMask permissions) {
    return set_permissions_impl(permissions, false);
}

void QuicSession::revoke_policy_permissions(PermissionMask permissions) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server || permissions == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.pending_policy_revocations |= permissions;
    impl.session_permissions.fetch_and(~permissions);
}

Error QuicSession::apply_permission_policy() {
    return set_permissions_impl(0, true);
}

Error QuicSession::set_permissions_impl(PermissionMask permissions, bool policy_change) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "set_permissions is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream was not announced yet");
    }
    if (!policy_change && !impl.server_config.allow_permission_change) {
        return Error(Status::InvalidCall, "permission changes are disabled on this host");
    }
    PermissionMask next = 0;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const PermissionMask revoked = impl.pending_policy_revocations;
        next = (policy_change ? impl.session_permissions.load() : permissions) &
            impl.permission_ceiling & impl.config_permissions() & ~revoked;
        if (next == impl.session_permissions.load() && revoked == 0) {
            return Error::success();
        }
        impl.pending_policy_revocations = 0;
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
        std::lock_guard<std::mutex> lock(impl.mutex);
        HQUIC handle = impl.microphone.exchange(nullptr);
        impl.microphone_finished = true;
        impl.microphone_received.clear();
        impl.microphone_parser.reset();
        impl.microphone_info = AudioInfo{};
        impl.microphone_format_changed = false;
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
            impl.clipboard_updates.clear();
            impl.clipboard_file_reads.clear(); impl.clipboard_file_data.clear();
            impl.requested_clipboard_reads.clear(); impl.accepted_clipboard_reads.clear();
            impl.abort_streams_locked(aborts);
        }
    }
    {
        std::lock_guard lock(impl.mutex);
        impl.revoke_file_permissions_locked();
    }
    if ((next & kPermissionPortForward) == 0) {
        // The targets and connections are host authority; drop them before
        // the announcement so a message that races the update cannot reopen
        // anything.
        std::lock_guard<std::mutex> lock(impl.mutex);
        const std::vector<HQUIC> aborts =
            impl.clear_port_forward_locked("port forwarding was revoked");
        impl.abort_streams_locked(aborts);
    }
    if ((next & kPermissionTerminal) == 0) {
        // Same for terminals: the console belongs to the host application,
        // which learns about the stop through take_terminal_stop().
        std::lock_guard<std::mutex> lock(impl.mutex);
        const std::vector<HQUIC> aborts =
            impl.clear_terminals_locked("terminal access was revoked");
        impl.abort_streams_locked(aborts);
    }
    if ((next & kPermissionCamera) == 0) {
        // The host owns the camera; queueing the stop wakes its capture loop
        // and the aborted stream ends the viewer's picture at once.
        HQUIC stream = nullptr;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            stream = impl.camera.load();
            if (impl.server_camera_id != 0) {
                while (impl.camera_stops.size() >= 4) {
                    impl.camera_stops.pop_front();
                }
                impl.camera_stops.push_back(impl.server_camera_id);
            }
            impl.camera_finished = true;
            impl.server_camera_id = 0;
            impl.server_pending_camera = 0;
            if (stream) {
                impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            }
        }
    }
    if ((next & kPermissionMicrophone) == 0) {
        // The host owns the microphone; a queued stop wakes its relay, which
        // stops the capture and closes the call for the viewer.
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.server_voice_active || impl.server_voice_pending != 0) {
            impl.server_voice_active = false;
            impl.server_voice_pending = 0;
            VoiceCallRequest stop;
            stop.connect = false;
            stop.request_id = ++impl.next_voice_request_id;
            if (impl.voice_requests.size() >= 4) {
                impl.voice_requests.pop_front();
            }
            impl.voice_requests.push_back(stop);
            impl.cv.notify_all();
        }
    }
    return impl.send_bytes(impl.control.load(), make_permission_update(next),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::set_permission_ceiling(PermissionMask permissions) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "set_permission_ceiling is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream was not announced yet");
    }
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.permission_ceiling = permissions & impl.config_permissions();
    return Error::success();
}

bool QuicSession::take_permission_update(PermissionMask& permissions) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->permission_update_ready) {
        return false;
    }
    permissions = impl_->effective_permissions();
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

bool QuicSession::take_ticket_renew(TicketRenewRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->ticket_renew_ready) {
        return false;
    }
    request = std::move(impl_->ticket_renew);
    impl_->ticket_renew = TicketRenewRequest{};
    impl_->ticket_renew_ready = false;
    return true;
}

Error QuicSession::send_ticket_renew_result(const TicketRenewResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_ticket_renew_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(), make_ticket_renew_result(result),
                           QUIC_SEND_FLAG_NONE);
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
    const PermissionMask granted = impl.effective_permissions();
    ack.input_enabled = (granted & kPermissionInput) != 0;
    ack.clipboard_enabled = (granted & kPermissionClipboard) != 0;
    ack.clipboard_direction = impl.server_config.clipboard_direction;
    ack.file_transfer_enabled = (granted & kPermissionFileTransfer) != 0;
    ack.microphone_enabled = (granted & kPermissionMicrophone) != 0;
    ack.privacy_mode_enabled = (granted & kPermissionPrivacyMode) != 0;
    ack.port_forward_enabled = (granted & kPermissionPortForward) != 0;
    ack.terminal_enabled = (granted & kPermissionTerminal) != 0;
    ack.terminal_persist = ack.terminal_persist && ack.terminal_enabled;
    ack.camera_enabled = (granted & kPermissionCamera) != 0;
    ack.record_enabled = (granted & kPermissionRecord) != 0;
    ack.restart_enabled = (granted & kPermissionRestart) != 0;
    ack.block_input_enabled = (granted & kPermissionBlockInput) != 0;
    ack.os_login_enabled = (granted & kPermissionOsLogin) != 0;
    ack.session_select_enabled = (granted & kPermissionSessionSelect) != 0;
    ack.remote_config_enabled = (granted & kPermissionRemoteConfig) != 0;
    ack.audio.enabled = ack.audio.enabled && (granted & kPermissionAudio) != 0;
    if (!ack.audio.enabled) {
        ack.audio.extradata.clear();
    }
    if (ack.files_only || ack.control_only) {
        // There is no picture and no media stream in a control-only session.
        // Normalize the message here so a caller that reused a desktop
        // description cannot announce a stream the viewer would wait for.
        ack.audio.enabled = false;
        ack.audio.extradata.clear();
        ack.width = 0;
        ack.height = 0;
        ack.source_width = 0;
        ack.source_height = 0;
        ack.displays.clear();
        ack.merged = false;
    }

    // Open both media streams before the HelloAck so the announcement never
    // describes a stream that failed to open. The client classifies them by
    // packet magic, so their order here does not matter.
    if (!ack.files_only && !ack.control_only) {
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
    }

    // v35: latch the audio transport once. The announcement and every later
    // send use the same decision, so a late DATAGRAM_STATE_CHANGED cannot
    // move audio between transports mid-session. Without a viewer
    // capability, peer datagram support, or a large enough datagram budget,
    // audio stays on the reliable stream.
    bool audio_datagram =
        ack.audio.enabled && impl.client_audio_datagram.load() &&
        impl.datagram_send_enabled.load() &&
        impl.datagram_max_send_length.load() >= kMinAudioDatagramBudget;
    if (audio_datagram) {
        // The stream carries no packets in this mode, but the viewer
        // classifies server streams by their leading magic and the FIN that
        // ends audio travels here. Send the magic once so the stream stays
        // identifiable; on failure fall back to the reliable transport.
        static const uint8_t kAudioMagic[4] = {'C', 'A', 'U', 'D'};
        if (!impl.try_send_bytes(impl.audio.load(),
                                 std::vector<uint8_t>(kAudioMagic, kAudioMagic + 4),
                                 QUIC_SEND_FLAG_NONE)) {
            audio_datagram = false;
        }
    }
    ack.audio_datagram = audio_datagram;
    impl.audio_datagram_active.store(audio_datagram);

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
    const PermissionMask granted = impl.effective_permissions();
    update.input_enabled = (granted & kPermissionInput) != 0;
    update.clipboard_enabled = (granted & kPermissionClipboard) != 0;
    update.clipboard_direction = impl.server_config.clipboard_direction;
    update.file_transfer_enabled = (granted & kPermissionFileTransfer) != 0;
    update.microphone_enabled = (granted & kPermissionMicrophone) != 0;
    update.privacy_mode_enabled = (granted & kPermissionPrivacyMode) != 0;
    update.port_forward_enabled = (granted & kPermissionPortForward) != 0;
    update.terminal_enabled = (granted & kPermissionTerminal) != 0;
    update.terminal_persist = update.terminal_persist && update.terminal_enabled;
    update.camera_enabled = (granted & kPermissionCamera) != 0;
    update.record_enabled = (granted & kPermissionRecord) != 0;
    update.restart_enabled = (granted & kPermissionRestart) != 0;
    update.block_input_enabled = (granted & kPermissionBlockInput) != 0;
    update.os_login_enabled = (granted & kPermissionOsLogin) != 0;
    update.session_select_enabled = (granted & kPermissionSessionSelect) != 0;
    update.remote_config_enabled = (granted & kPermissionRemoteConfig) != 0;
    bool audio_stream_open = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        audio_stream_open = impl.audio.load() != nullptr && !impl.audio_stopped;
    }
    if (update.audio.enabled && (granted & kPermissionAudio) != 0 &&
        !info.files_only && !info.control_only && !impl.audio.load()) {
        // Audio may first become available after admission (for example when
        // a follower takes over capture or the host enables system sound).
        // The transport decision remains the one latched by announce().
        HQUIC audio = nullptr;
        QUIC_STATUS status = impl.api->StreamOpen(
            impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            &Impl::stream_callback, &impl, &audio);
        if (QUIC_SUCCEEDED(status)) {
            status = impl.api->StreamStart(audio, QUIC_STREAM_START_FLAG_NONE);
        }
        if (QUIC_FAILED(status)) {
            if (audio) impl.api->StreamClose(audio);
        } else {
            impl.audio = audio;
            audio_stream_open = true;
        }
    }
    update.audio.enabled =
        update.audio.enabled && (granted & kPermissionAudio) != 0 && audio_stream_open;
    update.audio_datagram = impl.audio_datagram_active.load();
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
        if (impl.client_config.session_mode != SessionMode::Desktop) {
            return Error(Status::InvalidCall, "display selection requires a desktop session");
        }
    }
    return impl.send_bytes(impl.control.load(), make_switch_display(display_index),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_capture_displays(const CaptureDisplaysRequest& request) {
    auto& impl = *impl_;
    if (impl.role != Impl::Role::Client) return Error(Status::InvalidCall, "capture displays is a client operation");
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete || impl.client_config.session_mode != SessionMode::Desktop)
            return Error(Status::InvalidCall, "display subscriptions require a desktop handshake");
    }
    auto message = make_capture_displays(request);
    if (message.empty()) return Error(Status::InvalidCall, "too many display indices");
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_capture_displays_request(CaptureDisplaysRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->capture_display_requests.empty()) return false;
    request = std::move(impl_->capture_display_requests.front());
    impl_->capture_display_requests.pop_front();
    return true;
}

Error QuicSession::send_capture_displays_result(const CaptureDisplaysResult& result) {
    auto& impl = *impl_;
    if (impl.role != Impl::Role::Server || !impl.announced || impl.requested_mode != SessionMode::Desktop)
        return Error(Status::InvalidCall, "display subscription results require an announced desktop");
    auto message = make_capture_displays_result(result);
    if (message.empty()) return Error(Status::InvalidCall, "invalid display subscription result");
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_capture_displays_result(CaptureDisplaysResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->capture_display_results.empty()) return false;
    result = std::move(impl_->capture_display_results.front());
    impl_->capture_display_results.pop_front();
    return true;
}

Error QuicSession::refresh_display_stream(const DisplayStreamInfo& info) {
    auto& impl = *impl_;
    if (impl.role != Impl::Role::Server || !impl.announced || impl.requested_mode != SessionMode::Desktop)
        return Error(Status::InvalidCall, "display stream updates require an announced desktop");
    auto message = make_display_stream_info(info);
    if (message.empty()) return Error(Status::InvalidCall, "invalid display stream description");
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_display_stream_update(DisplayStreamInfo& info) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->display_stream_updates.empty()) return false;
    auto first = impl_->display_stream_updates.begin();
    info = std::move(first->second);
    impl_->display_stream_updates.erase(first);
    return true;
}

Error QuicSession::request_display_keyframe(uint32_t stream_id) {
    auto& impl = *impl_;
    if (impl.role != Impl::Role::Client) return Error(Status::InvalidCall, "display keyframes are a client operation");
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete || impl.client_config.session_mode != SessionMode::Desktop)
            return Error(Status::InvalidCall, "display keyframes require a desktop handshake");
    }
    return impl.send_bytes(impl.control.load(), make_display_keyframe_request(stream_id), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_display_keyframe_request(uint32_t& stream_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->display_keyframe_requests.empty()) return false;
    stream_id = impl_->display_keyframe_requests.front();
    impl_->display_keyframe_requests.pop_front();
    return true;
}

bool QuicSession::take_screenshot_request(ScreenshotRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->role != Impl::Role::Server || impl_->screenshot_requests.empty()) {
        return false;
    }
    request = impl_->screenshot_requests.front();
    impl_->screenshot_requests.pop_front();
    return true;
}

Error QuicSession::send_screenshot_response(const ScreenshotResponse& response) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server)
        return Error(Status::InvalidCall, "send_screenshot_response is a server operation");
    auto payload = make_screenshot_response(response);
    if (payload.empty()) return Error(Status::InvalidCall, "invalid screenshot response");
    std::lock_guard lock(impl.mutex);
    if (!impl.announced) return Error(Status::InvalidCall, "the stream is not open");
    HQUIC stream = impl.outgoing_screenshot;
    if (!stream) {
        if (response.index != 0) return Error(Status::Unavailable, "screenshot stream ended");
        const auto opened = impl.api->StreamOpen(impl.connection.load(),
            QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, &Impl::stream_callback, &impl, &stream);
        if (QUIC_FAILED(opened)) return quic_error(opened, "QUIC StreamOpen(screenshot)");
        impl.one_shot_streams.push_back(stream);
        impl.outgoing_screenshot = stream;
        impl.outgoing_screenshot_request = response.request_id;
        impl.outgoing_screenshot_next = 0;
        const uint16_t priority = 0;
        impl.api->SetParam(stream, QUIC_PARAM_STREAM_PRIORITY, sizeof(priority), &priority);
    }
    if (impl.outgoing_screenshot_request != response.request_id ||
        impl.outgoing_screenshot_next != response.index)
        return Error(Status::InvalidCall, "screenshot slices are out of order");
    auto message = std::move(payload);
    if (response.index == 0) message.insert(message.begin(), {'C', 'S', 'H', 'T'});
    const bool last = response.code != ScreenshotCode::Ok || response.index + 1 == response.count;
    const auto flags = static_cast<QUIC_SEND_FLAGS>((response.index == 0 ? QUIC_SEND_FLAG_START : 0) |
                                                   (last ? QUIC_SEND_FLAG_FIN : 0));
    auto sent = impl.send_bytes_impl(stream, std::move(message), flags, false, kNoSendDeadline, true);
    if (sent.ok()) {
        ++impl.outgoing_screenshot_next;
        if (last) impl.outgoing_screenshot = nullptr;
    } else if (sent.status() != Status::WaitTimeout) {
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        impl.outgoing_screenshot = nullptr;
    }
    return sent;
}

Error QuicSession::request_screenshot(uint32_t stream_id) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_screenshot is a client operation");
    }
    ScreenshotRequest request;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if (impl.client_screenshot_request_id != 0) {
            return Error(Status::InvalidCall, "a screenshot is already pending");
        }
        request.request_id = ++impl.next_screenshot_request_id;
        request.stream_id = stream_id;
        impl.client_screenshot_request_id = request.request_id;
        // A timed-out request may still have delivered its last slice; never
        // hand that picture to the next request.
        impl.screenshots.clear();
    }
    const Error sent = impl.send_bytes(impl.control.load(), make_screenshot_request(request),
                                       QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.client_screenshot_request_id == request.request_id) {
            impl.client_screenshot_request_id = 0;
        }
    }
    return sent;
}

bool QuicSession::take_screenshot(ScreenshotResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->screenshots.empty()) {
        return false;
    }
    result = std::move(impl_->screenshots.front());
    impl_->screenshots.pop_front();
    return true;
}

void QuicSession::cancel_screenshot() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->role != Impl::Role::Client) {
        return;
    }
    for (const auto& entry : impl_->screenshot_streams)
        impl_->api->StreamShutdown(entry.stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    impl_->client_screenshot_request_id = 0;
    impl_->screenshot_total = 0;
    impl_->screenshot_count = 0;
    impl_->screenshot_next = 0;
    impl_->screenshot_bytes.clear();
    impl_->screenshots.clear();
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
        if ((impl.effective_permissions() & kPermissionInput) == 0) {
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

bool QuicSession::take_privacy_mode_request(bool& enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->privacy_mode_request_ready) {
        return false;
    }
    impl_->privacy_mode_request_ready = false;
    if ((impl_->effective_permissions() & kPermissionPrivacyMode) == 0) {
        // The permission was revoked between the request and this drain; the
        // host must not act on a request it can no longer honor.
        return false;
    }
    enabled = impl_->privacy_mode_requested;
    return true;
}

Error QuicSession::send_privacy_mode_state(bool enabled, PrivacyModeCode code,
                                           const std::string& reason) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_privacy_mode_state is a server operation");
    }
    if (enabled && code != PrivacyModeCode::Ok) {
        return Error(Status::InvalidCall, "a failed privacy state cannot be enabled");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(), make_privacy_mode_state(enabled, code, reason),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::send_desktop_state(DesktopKind kind) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_desktop_state is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(), make_desktop_state(kind),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::set_privacy_mode(bool enabled) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "set_privacy_mode is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionPrivacyMode) == 0) {
            return Error(Status::PermissionDenied, "privacy mode is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(), make_set_privacy_mode(enabled),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_privacy_mode_state(bool& enabled, PrivacyModeCode& code,
                                          std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->privacy_mode_state_ready) {
        return false;
    }
    enabled = impl_->privacy_mode_enabled;
    code = impl_->privacy_mode_code;
    reason = impl_->privacy_mode_reason;
    impl_->privacy_mode_state_ready = false;
    return true;
}

bool QuicSession::take_desktop_state(DesktopKind& kind) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->desktop_state_ready) {
        return false;
    }
    kind = impl_->desktop_kind;
    impl_->desktop_state_ready = false;
    return true;
}

bool QuicSession::take_sas_request() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sas_requests_pending == 0) {
        return false;
    }
    --impl_->sas_requests_pending;
    return true;
}

uint64_t QuicSession::sas_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sas_requests_seen;
}

Error QuicSession::send_sas_result(SasCode code, const std::string& reason) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_sas_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(), make_sas_result(code, reason),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_sas() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_sas is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionInput) == 0) {
            return Error(Status::PermissionDenied, "input control is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(), make_send_sas(), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_sas_result(SasCode& code, std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->sas_result_ready) {
        return false;
    }
    code = impl_->sas_code;
    reason = impl_->sas_reason;
    impl_->sas_result_ready = false;
    return true;
}

uint64_t QuicSession::sas_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sas_results_seen;
}

bool QuicSession::take_elevation_request(uint32_t& request_id) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->elevation_requests.empty()) return false;
    request_id = impl_->elevation_requests.front();
    impl_->elevation_requests.pop_front();
    return true;
}
Error QuicSession::send_elevation_state(const ElevationState& state) {
    if (impl_->role != Impl::Role::Server) return Error(Status::InvalidCall, "not a host");
    auto bytes = make_elevation_state(state);
    if (bytes.empty()) return Error(Status::InvalidCall, "invalid elevation state");
    return impl_->send_bytes(impl_->control.load(), bytes, QUIC_SEND_FLAG_NONE);
}
Error QuicSession::request_elevation(uint32_t request_id) {
    if (impl_->role != Impl::Role::Client) return Error(Status::InvalidCall, "not a viewer");
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->hello_complete) return Error(Status::InvalidCall, "the handshake has not completed");
        if (!(impl_->effective_permissions() & kPermissionInput))
            return Error(Status::PermissionDenied, "elevation requires input control");
    }
    auto bytes = make_elevation_request(request_id);
    if (bytes.empty()) return Error(Status::InvalidCall, "invalid elevation request");
    return impl_->send_bytes(impl_->control.load(), bytes, QUIC_SEND_FLAG_NONE);
}
bool QuicSession::take_elevation_state(ElevationState& state) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->elevation_states.empty()) return false;
    state = std::move(impl_->elevation_states.front());
    impl_->elevation_states.pop_front();
    return true;
}

bool QuicSession::take_host_action_request(HostControlRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->host_action_requests.empty()) {
        return false;
    }
    request = impl_->host_action_requests.front();
    impl_->host_action_requests.pop_front();
    return true;
}

uint64_t QuicSession::host_action_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->host_action_requests_seen;
}

bool QuicSession::take_remote_config_request(RemoteConfigRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->remote_config_request_ready) {
        return false;
    }
    request = impl_->remote_config_request;
    impl_->remote_config_request_ready = false;
    return true;
}

uint64_t QuicSession::remote_config_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->remote_config_requests_seen;
}

Error QuicSession::send_remote_config_state(const RemoteConfigState& state) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_remote_config_state is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    const std::vector<uint8_t> payload = make_remote_config_state(state);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "remote config state is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_remote_config(uint32_t keys, uint32_t values) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_remote_config is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionRemoteConfig) == 0) {
            return Error(Status::PermissionDenied,
                         "remote configuration is disabled by the host");
        }
    }
    RemoteConfigRequest request;
    request.keys = keys;
    request.values = values;
    const std::vector<uint8_t> payload = make_remote_config_request(request);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "remote config request is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_remote_config_state(RemoteConfigState& state) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->remote_config_state_ready) {
        return false;
    }
    state = std::move(impl_->remote_config_state);
    impl_->remote_config_state_ready = false;
    return true;
}

uint64_t QuicSession::remote_config_states() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->remote_config_states_seen;
}

bool QuicSession::take_display_control_request(DisplayControlRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_control_request_ready) {
        return false;
    }
    request = impl_->display_control_request;
    impl_->display_control_request_ready = false;
    return true;
}

uint64_t QuicSession::display_control_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->display_control_requests_seen;
}

Error QuicSession::send_display_control_state(const DisplayControlState& state) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_display_control_state is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    const std::vector<uint8_t> payload = make_display_control_state(state);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "display control state is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_display_control(const DisplayControlRequest& request) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_display_control is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if (impl.requested_mode != SessionMode::Desktop) {
            return Error(Status::PermissionDenied,
                         "display control requires a desktop session");
        }
        const uint16_t needed =
            (request.action == DisplayControlAction::SetCount ||
             request.action == DisplayControlAction::ToggleOutput)
                ? kPermissionPrivacyMode | kPermissionInput
                : kPermissionPrivacyMode;
        if (request.action != DisplayControlAction::Query &&
            request.action != DisplayControlAction::Restore &&
            (impl.effective_permissions() & needed) != needed) {
            return Error(Status::PermissionDenied, "display control is not permitted");
        }
    }
    const std::vector<uint8_t> payload = make_display_control(request);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "display control request is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_display_control_state(DisplayControlState& state) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_control_state_ready) {
        return false;
    }
    state = std::move(impl_->display_control_state);
    impl_->display_control_state_ready = false;
    return true;
}

Error QuicSession::send_host_action_result(const HostControlResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_host_action_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    const std::vector<uint8_t> payload = make_host_action_result(result);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "host action result is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_host_action(HostAction action, bool block_input) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_host_action is a client operation");
    }
    if (action != HostAction::Restart && action != HostAction::LockScreen &&
        action != HostAction::BlockInput) {
        return Error(Status::InvalidCall, "unknown host action");
    }
    if (action != HostAction::BlockInput && block_input) {
        return Error(Status::InvalidCall, "only block input carries a value");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        const PermissionMask permissions = impl.effective_permissions();
        if (action == HostAction::Restart) {
            if ((permissions & kPermissionRestart) == 0) {
                return Error(Status::PermissionDenied,
                             "restart is disabled by the host");
            }
        } else if (action == HostAction::LockScreen) {
            if ((permissions & kPermissionInput) == 0) {
                return Error(Status::PermissionDenied,
                             "input control is disabled by the host");
            }
        } else if ((permissions & (kPermissionInput | kPermissionBlockInput)) !=
                   (kPermissionInput | kPermissionBlockInput)) {
            return Error(Status::PermissionDenied,
                         "blocking local input is disabled by the host");
        }
    }
    HostControlRequest request;
    request.action = action;
    request.block_input = block_input;
    return impl.send_bytes(impl.control.load(), make_host_action_request(request),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_host_action_result(HostControlResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->host_action_results.empty()) {
        return false;
    }
    result = std::move(impl_->host_action_results.front());
    impl_->host_action_results.pop_front();
    return true;
}

uint64_t QuicSession::host_action_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->host_action_results_seen;
}

bool QuicSession::take_os_login_request(std::string& password) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->os_login_requests.empty()) {
        return false;
    }
    password = std::move(impl_->os_login_requests.front());
    impl_->os_login_requests.pop_front();
    return true;
}

uint64_t QuicSession::os_login_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->os_login_requests_seen;
}

Error QuicSession::send_os_login_result(OsLoginCode code, const std::string& reason) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_os_login_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    const std::vector<uint8_t> payload = make_os_login_result(code, reason);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "OS login result is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

Error QuicSession::inject_login_text(const std::string& password) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "inject_login_text is a server operation");
    }
    if (!impl.injector) {
        return Error(Status::Unavailable, "input injection is not available");
    }
    // A modifier the viewer still holds must not ride along into the password
    // box: queue the release first, then the text and Return behind it.
    impl.injector->release_held();
    impl.injector->submit_text(password + "\n");
    return Error::success();
}

Error QuicSession::request_os_login(const std::string& password) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_os_login is a client operation");
    }
    const std::vector<uint8_t> payload = make_os_login_request(password);
    if (payload.empty()) {
        return Error(Status::InvalidCall,
                     "the OS password is empty, too long or contains a control character");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        const PermissionMask permissions = impl.effective_permissions();
        if ((permissions & (kPermissionInput | kPermissionOsLogin)) !=
            (kPermissionInput | kPermissionOsLogin)) {
            return Error(Status::PermissionDenied, "OS login is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_os_login_result(OsLoginCode& code, std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->os_login_result_ready) {
        return false;
    }
    code = impl_->os_login_code;
    reason = impl_->os_login_reason;
    impl_->os_login_result_ready = false;
    return true;
}

uint64_t QuicSession::os_login_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->os_login_results_seen;
}

bool QuicSession::take_session_list_request() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session_list_request_pending) {
        return false;
    }
    impl_->session_list_request_pending = false;
    return true;
}

uint64_t QuicSession::session_list_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session_list_requests_seen;
}

Error QuicSession::send_session_list_result(const SessionListResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_session_list_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    const std::vector<uint8_t> payload = make_session_list_result(result);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "session list result is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_session_select_request(uint32_t& session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session_select_request_ready) {
        return false;
    }
    session_id = impl_->session_select_request_id;
    impl_->session_select_request_ready = false;
    return true;
}

uint64_t QuicSession::session_select_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session_select_requests_seen;
}

Error QuicSession::send_session_select_result(const SessionSelectResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_session_select_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    const std::vector<uint8_t> payload = make_session_select_result(result);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "session select result is invalid");
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_session_list() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_session_list is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionSessionSelect) == 0) {
            return Error(Status::PermissionDenied, "session select is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(), make_session_list_request(),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_session_list_result(SessionListResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session_list_result_ready) {
        return false;
    }
    result = std::move(impl_->session_list_result);
    impl_->session_list_result_ready = false;
    return true;
}

uint64_t QuicSession::session_list_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session_list_results_seen;
}

Error QuicSession::request_session_select(uint32_t session_id) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_session_select is a client operation");
    }
    const std::vector<uint8_t> payload = make_session_select_request(session_id);
    if (payload.empty()) {
        return Error(Status::InvalidCall, "the session id is invalid");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionSessionSelect) == 0) {
            return Error(Status::PermissionDenied, "session select is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(), payload, QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_session_select_result(SessionSelectResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session_select_result_ready) {
        return false;
    }
    result = impl_->session_select_result;
    impl_->session_select_result_ready = false;
    return true;
}

uint64_t QuicSession::session_select_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session_select_results_seen;
}

bool QuicSession::take_stream_options_request(StreamOptions& options) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->stream_options_request_ready) {
        return false;
    }
    options = impl_->stream_options_request;
    impl_->stream_options_request_ready = false;
    return true;
}

uint64_t QuicSession::stream_options_requests() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stream_options_requests_seen;
}

Error QuicSession::send_stream_options_result(const StreamOptionsResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_stream_options_result is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(), make_stream_options_result(result),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_stream_options(const StreamOptions& options) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_stream_options is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
    }
    return impl.send_bytes(impl.control.load(), make_set_stream_options(options),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_stream_options_result(StreamOptionsResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->stream_options_result_ready) {
        return false;
    }
    result = impl_->stream_options_result;
    impl_->stream_options_result_ready = false;
    return true;
}

uint64_t QuicSession::stream_options_results() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stream_options_results_seen;
}

Error QuicSession::send_frame(FrameHeader header, const uint8_t* payload, size_t size) {
    return send_frame_impl(header, payload, size, true);
}

Error QuicSession::send_frame_nowait(FrameHeader header, const uint8_t* payload, size_t size) {
    return send_frame_impl(header, payload, size, false);
}

Error QuicSession::send_frame_best_effort(FrameHeader header, const uint8_t* payload, size_t size) {
    return send_frame_impl(header, payload, size, false, false);
}

bool QuicSession::video_send_ready() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->video_capacity_locked() && impl_->has_send_capacity(1);
}

Error QuicSession::send_frame_impl(FrameHeader header, const uint8_t* payload, size_t size,
                                   bool wait, bool track) {
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
    std::unique_lock<std::mutex> sending(impl.video_send_mutex);
    const auto bytes_needed = kFrameHeaderSize + size;
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        if (!valid_video_format(header.codec, header.chroma) ||
            !(impl.decoding_codecs & video_codec_bit(header.codec)) ||
            (header.chroma == VideoChroma::Yuv444 && !(impl.decoding_444 & video_codec_bit(header.codec))))
            return Error(Status::Unsupported, "the viewer cannot decode this video format");
        // Refinements carry no new content: they neither claim delivery-window
        // credit nor wait for a stalled viewer. Transport send capacity still
        // bounds them below, so a frozen display cannot grow the queue.
        const auto ready = [&] {
            if (!track && !wait) return true;
            return impl.video_capacity_locked(bytes_needed) || impl.transport_down.load();
        };
        if (wait) {
            if (!impl.cv.wait_for(lock, kSendCapacityTimeout, ready))
                return Error(Status::WaitTimeout, "video delivery window did not drain");
        } else if (!ready()) {
            return Error(Status::WaitTimeout, "video delivery window is full");
        }
        if (impl.transport_down.load()) return Error(Status::Unavailable, "connection closed before video send");
    }
    header.seq = impl.next_seq.load();
    header.payload_size = static_cast<uint32_t>(size);
    header.payload_crc32 = crc32(payload, size);
    std::vector<uint8_t> bytes(kFrameHeaderSize + size);
    encode_frame_header(header, bytes.data());
    std::memcpy(bytes.data() + kFrameHeaderSize, payload, size);
    if (track) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto now = steady_us();
        const auto captured = header.capture_ms && header.capture_ms <= now / 1000 ? header.capture_ms * 1000 : now;
        impl.pending_video.push_back({header.seq, now, captured, bytes.size()});
        impl.pending_video_bytes += bytes.size();
    }
    auto sent = impl.send_bytes_impl(video, std::move(bytes), QUIC_SEND_FLAG_NONE, wait);
    if (sent.ok()) {
        ++impl.next_seq;
    } else if (track) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.pending_video.empty() && impl.pending_video.back().sequence == header.seq) {
            impl.pending_video_bytes -= impl.pending_video.back().bytes;
            impl.pending_video.pop_back();
        }
    }
    return sent;
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

bool QuicSession::take_display_follow_request(uint8_t& flags) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_follow_request_ready) {
        return false;
    }
    flags = impl_->display_follow_flags;
    impl_->display_follow_request_ready = false;
    return true;
}

Error QuicSession::request_display_follow(uint8_t flags) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_display_follow is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionInput) == 0) {
            return Error(Status::PermissionDenied, "input is disabled by the host");
        }
    }
    return impl.send_bytes(impl.control.load(), make_set_display_follow(flags),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_display_follow_state(DisplayFollowState& state) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->display_follow_state_ready) {
        return false;
    }
    state = impl_->display_follow_state;
    impl_->display_follow_state_ready = false;
    return true;
}

Error QuicSession::send_display_follow_state(const DisplayFollowState& state) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_display_follow_state is a server operation");
    }
    if (!impl.announced) {
        return Error(Status::InvalidCall, "the stream is not open");
    }
    return impl.send_bytes(impl.control.load(), make_display_follow_state(state),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_show_cursor_hint(ShowCursorHint& hint) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->cursor_hint_ready) {
        return false;
    }
    hint = impl_->cursor_hint;
    impl_->cursor_hint_ready = false;
    return true;
}

uint64_t QuicSession::show_cursor_hints() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->cursor_hints_seen;
}

Error QuicSession::request_show_cursor(const ShowCursorHint& hint) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_show_cursor is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
    }
    return impl.send_bytes(impl.control.load(), make_show_cursor(hint),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::send_audio(AudioHeader header, const uint8_t* payload, size_t size) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_audio is a server operation");
    }
    // A closed gate is a caller mistake, not a lost stream; keep the two
    // cases distinguishable for the host and the loopback check.
    if ((impl.config_permissions() & kPermissionAudio) == 0) {
        return Error(Status::InvalidCall, "audio is disabled on this server");
    }
    // The operator may revoke audio mid-session. The stream stays open so a
    // restore resumes immediately, but nothing leaves the host in between.
    if ((impl.effective_permissions() & kPermissionAudio) == 0) {
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
    // with it, video. Dropping a packet is a short gap in the audio stream.
    if (impl.audio_datagram_active.load()) {
        return impl.send_audio_datagram(std::move(bytes));
    }
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

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // v62: a client-initiated stream is invisible to the host until its first
    // byte, so the viewer opens the exchange and the host answers with its
    // challenge. The possession proof is bound to that challenge.
    Error began = impl.send_bytes(control, make_control(ControlType::AuthBegin, {}),
                                  QUIC_SEND_FLAG_NONE);
    if (!began.ok()) {
        return began;
    }
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        impl.cv.wait_until(lock, deadline, [&impl] {
            return impl.auth_challenge_ready || impl.access_denied_ready ||
                   impl.transport_failed || impl.shutdown_complete || impl.peer_closed;
        });
        if (impl.access_denied_ready) {
            return Error(Status::PermissionDenied, impl.denial_reason);
        }
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (!impl.auth_challenge_ready) {
            if (impl.peer_closed || impl.shutdown_complete) {
                return Error(Status::Unavailable,
                             "the server closed the connection before its authentication challenge");
            }
            return Error(Status::WaitTimeout,
                         "the server did not send its authentication challenge");
        }
    }

    ClientHello hello;
    hello.display = display;
    hello.access_token = impl.client_config.access_token;
    hello.password = impl.client_config.password;
    hello.totp = impl.client_config.totp;
    hello.audit_ref = impl.client_config.audit_ref;
    hello.requested_permissions = impl.client_config.requested_permissions & kPermissionAll;
    hello.mode = impl.client_config.session_mode;
    hello.capabilities = impl.client_config.audio_datagram ? kClientCapabilityAudioDatagram : 0;
    hello.decoding_codecs = impl.client_config.decoding_codecs;
    hello.codec_preference = impl.client_config.codec_preference;
    hello.decoding_444 = impl.client_config.decoding_444;
    hello.prefer_chroma = impl.client_config.prefer_chroma;
    if (!secure_random(hello.nonce.data(), hello.nonce.size())) {
        return Error(Status::CaptureFailed, "cannot generate the Hello nonce");
    }
    hello.unix_seconds = static_cast<uint64_t>(std::time(nullptr));
    if (impl.client_config.device_identity) {
        const DeviceIdentity& identity = *impl.client_config.device_identity;
        hello.device_key = identity.public_key();
        hello.device_name = truncate_utf8(identity.name(), kMaxDeviceNameLength);
        hello.device_platform = static_cast<uint8_t>(impl.client_config.device_platform);
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
    if (!impl.client_config.access_ticket.empty()) {
        if (impl.client_config.access_ticket_key.size() != kTicketClientKeySize) {
            return Error(Status::InvalidCall,
                         "an access ticket needs its 32-byte Ed25519 client key");
        }
        if (impl.client_config.access_ticket.size() <= kTicketSignatureSize) {
            return Error(Status::InvalidCall, "the access ticket is malformed");
        }
        AccessTicket ticket;
        // decode_access_ticket takes the envelope; the verifier strips the
        // trailing Ed25519 signature the same way.
        const std::vector<uint8_t> envelope(
            impl.client_config.access_ticket.begin(),
            impl.client_config.access_ticket.end() - kTicketSignatureSize);
        if (decode_access_ticket(envelope, &ticket) != TicketError::None) {
            return Error(Status::InvalidCall, "the access ticket is malformed");
        }
        std::array<uint8_t, kTicketProofHostNonceSize> host_nonce{};
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            host_nonce = impl.host_nonce;
        }
        const std::vector<uint8_t> message = access_ticket_proof_message(
            ticket.host_id, ticket.host_public_key_fingerprint, host_nonce,
            std::vector<uint8_t>(hello.nonce.begin(), hello.nonce.end()),
            impl.client_config.access_ticket);
        if (!sign_access_ticket_proof(impl.client_config.access_ticket_key, message,
                                      &hello.ticket_proof)) {
            return Error(Status::CaptureFailed, "cannot sign the access-ticket proof");
        }
        hello.access_ticket = impl.client_config.access_ticket;
    } else if (!impl.client_config.access_ticket_key.empty()) {
        return Error(Status::InvalidCall, "an access-ticket key needs a ticket");
    }
    Error sent = impl.send_bytes(control, make_hello(hello), QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        return sent;
    }
    std::unique_lock<std::mutex> lock(impl.mutex);
    impl.cv.wait_until(lock, deadline, [&impl] {
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
    // A later renewal proof covers this nonce, so the host can tie the new
    // ticket to the connection that was admitted.
    impl.client_nonce = hello.nonce;
    return Error::success();
}

Error QuicSession::receive_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                                 std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "receive_frame is a client operation");
    }
    impl.flush_video_receipt();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    for (;;) {
        // Parsers are also reset by callbacks and teardown while holding the
        // lock, so pushing under the lock keeps vector growth from racing a
        // reset on another thread.
        while (!impl.received.empty()) {
            std::vector<uint8_t> chunk = impl.received.take_front();
            std::string error;
            if (!impl.frame_parser.push(chunk.data(), chunk.size(), &error)) {
                return Error(Status::CaptureFailed, "video stream: " + error);
            }
        }
        if (!impl.video_finished && !impl.shutdown_complete) {
            impl.resume_media_locked(impl.video.load(), impl.received,
                                     kVideoReceiveHighWaterBytes);
        }

        const uint8_t* frame_payload = nullptr;
        std::string parse_error;
        switch (impl.frame_parser.next(header, &frame_payload, &parse_error)) {
            case FrameParser::Status::Frame: {
                payload.assign(frame_payload, frame_payload + header.payload_size);
                impl.frame_parser.consume();
                impl.pending_video_receipt = header.seq;
                lock.unlock();
                impl.flush_video_receipt();
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
        // v35: datagrams are whole packets, so they bypass the byte-stream
        // parser. Serving them first also keeps the stream parser's buffer
        // contiguous when the host falls back to the stream.
        while (impl.peer_info.audio_datagram && !impl.audio_datagrams.empty()) {
            std::vector<uint8_t> packet = std::move(impl.audio_datagrams.front());
            impl.audio_datagrams.pop_front();
            AudioHeader datagram_header;
            std::string datagram_error;
            if (!decode_audio_header(packet.data(), packet.size(), datagram_header,
                                     &datagram_error) ||
                datagram_header.payload_size == 0 ||
                datagram_header.payload_size != packet.size() - kAudioHeaderSize) {
                continue;
            }
            header = datagram_header;
            payload.assign(packet.data() + kAudioHeaderSize, packet.data() + packet.size());
            return Error::success();
        }
        while (!impl.audio_received.empty()) {
            std::vector<uint8_t> chunk = impl.audio_received.take_front();
            std::string error;
            if (!impl.audio_parser.push(chunk.data(), chunk.size(), &error)) {
                return Error(Status::CaptureFailed, "audio stream: " + error);
            }
        }
        if (!impl.audio_finished && !impl.shutdown_complete) {
            impl.resume_media_locked(impl.audio.load(), impl.audio_received,
                                     kAudioReceiveHighWaterBytes);
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
            // In datagram mode the stream carries only the identification
            // magic, so a four-byte remainder is the expected tail.
            const size_t stream_tail = impl.audio_parser.buffered();
            const bool datagram_magic_only = impl.peer_info.audio_datagram && stream_tail <= 4;
            if (stream_tail > 0 && !datagram_magic_only) {
                return Error(Status::CaptureFailed, "audio stream ended mid-packet");
            }
            if (impl.peer_info.audio_datagram) {
                // The FIN and the datagrams take independent paths; give the
                // tail a bounded quiet period so a clean stop drains it.
                const auto now = std::chrono::steady_clock::now();
                if (impl.audio_datagram_drain_until ==
                    std::chrono::steady_clock::time_point{}) {
                    impl.audio_datagram_drain_until =
                        now + std::chrono::milliseconds(150);
                }
                if (now < impl.audio_datagram_drain_until && now < deadline) {
                    const auto wake =
                        deadline < impl.audio_datagram_drain_until ? deadline
                                                                   : impl.audio_datagram_drain_until;
                    impl.cv.wait_until(lock, wake);
                    continue;
                }
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
            std::vector<uint8_t> chunk = impl.microphone_received.take_front();
            std::string error;
            if (!impl.microphone_parser.push(chunk.data(), chunk.size(), &error)) {
                return Error(Status::CaptureFailed, "microphone stream: " + error);
            }
        }
        if (!impl.microphone_finished && !impl.shutdown_complete) {
            impl.resume_media_locked(impl.microphone.load(), impl.microphone_received,
                                     kMicrophoneReceiveHighWaterBytes);
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
        if ((impl.effective_permissions() & kPermissionMicrophone) == 0) {
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
    if ((impl.effective_permissions() & kPermissionMicrophone) == 0) {
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
    std::lock_guard<std::mutex> lock(impl.mutex);
    HQUIC microphone = impl.microphone.exchange(nullptr);
    if (!microphone) {
        return Error::success();
    }
    impl.microphone_received.clear();
    impl.microphone_parser.reset();
    impl.microphone_format_changed = false;
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
    const auto now = steady_us();
    auto timestamp = now;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) return Error(Status::InvalidCall, "handshake not complete");
        if (impl.role == Impl::Role::Server) {
            if (impl.pending_qos_probe_us) {
                // Callback replies are nonblocking and may have been dropped
                // by a full queue. Retry the same stamp, preserving its age.
                if (now - impl.last_qos_probe_send_us < 1000000) return Error::success();
                timestamp = impl.pending_qos_probe_us;
            } else {
                impl.pending_qos_probe_us = timestamp;
            }
            impl.last_qos_probe_send_us = now;
        }
    }
    Error sent = impl.send_bytes_impl(impl.control.load(), make_latency_probe(timestamp),
                                      QUIC_SEND_FLAG_NONE, false);
    if (sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        ++impl.latency.probes_sent;
    }
    return sent;
}

LatencySample QuicSession::latency() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto sample = impl_->latency;
    if (impl_->pending_qos_probe_us) sample.pending_us = steady_us() - impl_->pending_qos_probe_us;
    if (!impl_->pending_video.empty()) sample.video_pending_us = steady_us() - impl_->pending_video.front().capture_us;
    sample.video_pending_frames = impl_->pending_video.size();
    sample.video_pending_bytes = impl_->pending_video_bytes;
    return sample;
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
    if ((impl.effective_permissions() & kPermissionInput) == 0) {
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
        if (!impl.clipboard_allowed(true)) {
            return Error(Status::PermissionDenied, "clipboard is disabled for this session");
        }
    } else {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if (!impl.clipboard_allowed(true)) {
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
        if (!impl.clipboard_allowed(true)) {
            return Error(Status::PermissionDenied, "clipboard is disabled for this session");
        }
    } else {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if (!impl.clipboard_allowed(true)) {
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

Error QuicSession::request_clipboard_file(const ClipboardFileRead& request) {
    auto message = make_clipboard_file_read(request);
    if (message.empty()) return Error(Status::InvalidCall, "invalid clipboard file read");
    auto& impl = *impl_;
    {
        std::lock_guard lock(impl.mutex);
        if (!impl.clipboard_allowed(false)) return Error(Status::PermissionDenied, "clipboard receive is disabled");
        if (impl.requested_clipboard_reads.size() + impl.clipboard_file_data.size() >= 8 ||
            impl.requested_clipboard_reads.count(request.request_id))
            return Error(Status::Unavailable, "clipboard file read queue is full");
        impl.requested_clipboard_reads.emplace(request.request_id, request);
    }
    auto sent = impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) { std::lock_guard lock(impl.mutex); impl.requested_clipboard_reads.erase(request.request_id); }
    return sent;
}

Error QuicSession::send_clipboard_file_data(const ClipboardFileData& response) {
    auto message = make_clipboard_file_data(response);
    if (message.empty()) return Error(Status::InvalidCall, "invalid clipboard file data");
    auto& impl = *impl_;
    {
        std::lock_guard lock(impl.mutex);
        if (!impl.clipboard_allowed(true)) return Error(Status::PermissionDenied, "clipboard send is disabled");
        const auto found = impl.accepted_clipboard_reads.find(response.request_id);
        if (found == impl.accepted_clipboard_reads.end() || found->second.generation != response.generation ||
            response.bytes.size() > found->second.length)
            return Error(Status::InvalidCall, "clipboard data has no matching read");
        impl.accepted_clipboard_reads.erase(found);
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_clipboard_file_read(ClipboardFileRead& request) {
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    if (!impl.clipboard_allowed(true)) {
        impl.clipboard_file_reads.clear(); impl.accepted_clipboard_reads.clear(); return false;
    }
    if (impl.clipboard_file_reads.empty()) return false;
    request = impl.clipboard_file_reads.front(); impl.clipboard_file_reads.pop_front(); return true;
}

bool QuicSession::take_clipboard_file_data(ClipboardFileData& response) {
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    if (!impl.clipboard_allowed(false)) {
        impl.clipboard_file_data.clear(); impl.requested_clipboard_reads.clear(); return false;
    }
    if (impl.clipboard_file_data.empty()) return false;
    response = std::move(impl.clipboard_file_data.front()); impl.clipboard_file_data.pop_front(); return true;
}

void QuicSession::cancel_clipboard_file_reads(uint64_t generation) {
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->requested_clipboard_reads, [&](const auto& item) { return item.second.generation == generation; });
    std::erase_if(impl_->clipboard_file_data, [&](const auto& item) { return item.generation == generation; });
}

Error QuicSession::send_chat(const std::string& text) {
    Impl& impl = *impl_;
    if (impl.role == Impl::Role::Client) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
    }
    std::vector<uint8_t> message = make_chat_message(text);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid chat text");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_chat(std::string& text) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->chats.empty()) {
        return false;
    }
    text = std::move(impl_->chats.front());
    impl_->chats.pop_front();
    return true;
}

uint64_t QuicSession::chats_received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->chats_seen;
}

bool QuicSession::take_voice_call_request(VoiceCallRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->voice_requests.empty()) {
        return false;
    }
    request = impl_->voice_requests.front();
    impl_->voice_requests.pop_front();
    return true;
}

Error QuicSession::send_voice_call_response(const VoiceCallResponse& response) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_voice_call_response is a server operation");
    }
    VoiceCallResponse answer = response;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the stream is not open");
        }
        if (answer.request_id == 0 || answer.request_id != impl.server_voice_pending) {
            return Error(Status::InvalidCall, "no voice call request is waiting");
        }
        impl.server_voice_pending = 0;
        if (answer.code == VoiceCallCode::Ok) {
            if ((impl.effective_permissions() & kPermissionMicrophone) == 0) {
                answer.code = VoiceCallCode::Refused;
                answer.reason = "microphone access was revoked";
                answer.audio = AudioInfo{};
            } else {
                impl.next_voice_seq = 0;
            }
        }
        if (make_voice_call_response(answer).empty()) {
            return Error(Status::InvalidCall, "invalid voice call response");
        }
        impl.server_voice_active = answer.code == VoiceCallCode::Ok;
    }
    Error sent = impl.send_bytes(impl.control.load(), make_voice_call_response(answer),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.server_voice_active = false;
    }
    return sent;
}

Error QuicSession::send_voice_audio(const AudioHeader& header, const uint8_t* payload,
                                    size_t size) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_voice_audio is a server operation");
    }
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the stream is not open");
        }
        if ((impl.effective_permissions() & kPermissionMicrophone) == 0) {
            return Error(Status::PermissionDenied, "microphone access was revoked");
        }
        if (!impl.server_voice_active) {
            return Error(Status::Unavailable, "no voice call is running");
        }
        seq = impl.next_voice_seq++;
    }
    AudioHeader wire = header;
    wire.seq = seq;
    std::vector<uint8_t> message = make_voice_audio(wire, payload, size);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid voice audio packet");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

Error QuicSession::send_voice_call_close() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_voice_call_close is a server operation");
    }
    VoiceCallRequest request;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the stream is not open");
        }
        if (!impl.server_voice_active && impl.server_voice_pending == 0) {
            return Error::success();
        }
        impl.server_voice_active = false;
        impl.server_voice_pending = 0;
        request.request_id = ++impl.next_voice_request_id;
        request.connect = false;
    }
    return impl.send_bytes(impl.control.load(), make_voice_call_request(request),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::voice_call_active() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->role == Impl::Role::Server ? impl_->server_voice_active
                                             : impl_->client_voice_active;
}

Error QuicSession::request_voice_call() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_voice_call is a client operation");
    }
    VoiceCallRequest request;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionMicrophone) == 0) {
            return Error(Status::PermissionDenied, "the host did not grant the microphone");
        }
        if (impl.client_voice_active || impl.client_voice_requested) {
            return Error(Status::InvalidCall, "a voice call is already running");
        }
        request.request_id = ++impl.next_voice_request_id;
        request.connect = true;
        impl.client_voice_requested = true;
        impl.client_voice_request_id = request.request_id;
        impl.voice_closed_notice = false;
    }
    Error sent = impl.send_bytes(impl.control.load(), make_voice_call_request(request),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.client_voice_request_id == request.request_id) {
            impl.client_voice_requested = false;
            impl.client_voice_request_id = 0;
        }
    }
    return sent;
}

Error QuicSession::request_voice_call_stop() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_voice_call_stop is a client operation");
    }
    VoiceCallRequest request;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if (!impl.client_voice_active && !impl.client_voice_requested) {
            return Error::success();
        }
        impl.client_voice_active = false;
        impl.client_voice_requested = false;
        impl.client_voice_request_id = 0;
        impl.voice_audio.clear();
        impl.buffered_voice_bytes = 0;
        request.request_id = ++impl.next_voice_request_id;
        request.connect = false;
    }
    return impl.send_bytes(impl.control.load(), make_voice_call_request(request),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_voice_call_response(VoiceCallResponse& response) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->voice_responses.empty()) {
        return false;
    }
    response = std::move(impl_->voice_responses.front());
    impl_->voice_responses.pop_front();
    return true;
}

bool QuicSession::take_voice_call_closed() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool closed = impl_->voice_closed_notice;
    impl_->voice_closed_notice = false;
    return closed;
}

bool QuicSession::take_voice_audio(AudioHeader& header, std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    while (!impl_->voice_audio.empty()) {
        std::vector<uint8_t> raw = std::move(impl_->voice_audio.front());
        impl_->voice_audio.pop_front();
        impl_->buffered_voice_bytes -= raw.size();
        const uint8_t* audio = nullptr;
        size_t audio_size = 0;
        std::string error;
        if (!parse_voice_audio(raw, header, &audio, audio_size, &error)) {
            continue;
        }
        payload.assign(audio, audio + audio_size);
        return true;
    }
    return false;
}

Error QuicSession::send_text_input(const std::string& text) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "send_text_input is a viewer operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
    }
    if ((impl.effective_permissions() & kPermissionInput) == 0) {
        return Error(Status::PermissionDenied, "input is disabled by the host");
    }
    std::vector<uint8_t> message = make_text_input(text);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid text input");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_text_input(std::string& text) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->texts.empty()) {
        return false;
    }
    text = std::move(impl_->texts.front());
    impl_->texts.pop_front();
    return true;
}

uint64_t QuicSession::text_inputs_received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->texts_seen;
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
    {
        std::lock_guard lock(impl.mutex);
        const auto it = impl.incoming_file_offers.find(transfer_id);
        if (code == FileCode::Ok) {
            if (it == impl.incoming_file_offers.end() || !impl.file_allowed(it->second.clipboard, false)) {
                return Error(Status::PermissionDenied, "file offer is missing or permission was revoked");
            }
            it->second.accepted = true;
        } else if (code != FileCode::Conflict) {
            // Conflict keeps the offer held until the decision arrives.
            impl.incoming_file_offers.erase(transfer_id);
        }
    }
    if (code != FileCode::Ok && code != FileCode::Conflict) {
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
                    if (entry.second.receive_paused && entry.second.stream) {
                        entry.second.receive_paused = false;
                        impl.api->StreamReceiveSetEnabled(entry.second.stream, TRUE);
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
    state.wire_bytes = it->second.wire_bytes;
    state.compressed = it->second.compressed;
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
        impl.incoming_file_offers.erase(transfer_id);
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
        if (!impl.file_allowed(offer.clipboard, true)) {
            return Error(Status::PermissionDenied, "file transfer is disabled by the host");
        }
        if (impl.outgoing_uploads.count(offer.transfer_id) != 0) {
            return Error(Status::InvalidCall, "the transfer id is already in use");
        }
        // Offers that were refused, skipped or cancelled never produce a
        // FileResult, so their finished entries must not exhaust the slots.
        std::erase_if(impl.outgoing_uploads, [](const auto& item) {
            return item.second.finished && item.second.stream == nullptr;
        });
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
    // Conflict holds the transfer open until FileConflictAnswer arrives.
    if (answer.code != FileCode::Ok && answer.code != FileCode::Conflict) {
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
        if (!impl.file_allowed(upload.offer.clipboard, true)) {
            return Error(Status::PermissionDenied, "file or clipboard permission was disabled");
        }
        if (!upload.started) {
            QUIC_STATUS status = impl.api->StreamOpen(
                impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                &Impl::stream_callback, &impl, &upload.stream);
            if (QUIC_FAILED(status)) {
                return quic_error(status, "QUIC StreamOpen(upload)");
            }
            upload.started = true;
            starting = true;
            // A resumed transfer always uses the raw body: the receiver
            // already has bytes on disk and a v2 stream cannot restart block
            // framing at an arbitrary offset.
            upload.compress =
                upload.next_offset == 0 &&
                file_compression_worthwhile(upload.offer.name, upload.offer.size,
                                            upload.offer.clipboard);
            FileStreamHeader header;
            header.transfer_id = transfer_id;
            header.offset = upload.next_offset;
            if (upload.compress) {
                header.version = kFileStreamVersionCompressed;
            }
            bytes.resize(kFileStreamHeaderSize);
            encode_file_stream_header(header, bytes.data());
            if (upload.compress) {
                if (!append_compressed_file_block(data, size, bytes)) {
                    append_raw_file_block(data, size, bytes);
                }
            } else {
                bytes.insert(bytes.end(), data, data + size);
            }
        } else if (upload.compress) {
            if (!append_compressed_file_block(data, size, bytes)) {
                append_raw_file_block(data, size, bytes);
            }
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
    if (!impl.has_send_capacity(size + kFileStreamHeaderSize + kFileBlockHeaderSize)) {
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
        if (!impl.file_allowed(upload.offer.clipboard, true)) {
            return Error(Status::PermissionDenied, "file or clipboard permission was disabled");
        }
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
            header.version = upload.compress ? kFileStreamVersionCompressed
                                             : kFileStreamVersion;
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

Error QuicSession::send_file_conflict(const FileConflict& conflict) {
    Impl& impl = *impl_;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.role != Impl::Role::Server) {
            return Error(Status::InvalidCall, "only the host holds an offer for a decision");
        }
        const auto it = impl.incoming_file_offers.find(conflict.transfer_id);
        if (it == impl.incoming_file_offers.end() || it->second.accepted) {
            return Error(Status::InvalidCall, "the offer is not being held");
        }
        if (!impl.file_allowed(it->second.clipboard, false)) {
            return Error(Status::PermissionDenied,
                         "file transfer is disabled by the host");
        }
    }
    std::vector<uint8_t> message = make_file_conflict(conflict);
    if (message.empty()) {
        return Error(Status::InvalidCall, "the conflict is not encodable");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_file_conflict(FileConflict& conflict) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_conflicts.empty()) {
        return false;
    }
    conflict = impl_->file_conflicts.front();
    impl_->file_conflicts.pop_front();
    return true;
}

Error QuicSession::send_file_conflict_answer(const FileConflictAnswer& answer) {
    Impl& impl = *impl_;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.role != Impl::Role::Client) {
            return Error(Status::InvalidCall, "only the viewer answers a held offer");
        }
        const auto sent = impl.outgoing_uploads.find(answer.transfer_id);
        if (sent != impl.outgoing_uploads.end() && !sent->second.finished) {
            // A viewer upload the host is holding for the decision.
            if (!impl.file_allowed(sent->second.offer.clipboard, true)) {
                return Error(Status::PermissionDenied,
                             "file transfer is disabled by the host");
            }
        } else {
            // A host download we held with FileAnswer{Conflict}. Overwrite
            // re-arms the offer so the stream that follows is accepted.
            const auto offered = impl.incoming_file_offers.find(answer.transfer_id);
            if (offered == impl.incoming_file_offers.end() || offered->second.accepted) {
                return Error(Status::InvalidCall, "the transfer is not being held");
            }
            if (!impl.file_allowed(offered->second.clipboard, false)) {
                return Error(Status::PermissionDenied,
                             "file transfer is disabled by the host");
            }
            if (answer.decision == FileConflictDecision::Overwrite) {
                offered->second.accepted = true;
            } else {
                impl.incoming_file_offers.erase(offered);
            }
        }
    }
    std::vector<uint8_t> message = make_file_conflict_answer(answer);
    if (message.empty()) {
        return Error(Status::InvalidCall, "the answer is not encodable");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_file_conflict_answer(FileConflictAnswer& answer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_conflict_answers.empty()) {
        return false;
    }
    answer = impl_->file_conflict_answers.front();
    impl_->file_conflict_answers.pop_front();
    return true;
}

Error QuicSession::accept_held_download(uint32_t transfer_id) {
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "only the host sends a download");
    }
    const auto it = impl.outgoing_uploads.find(transfer_id);
    if (it == impl.outgoing_uploads.end() || it->second.finished) {
        return Error(Status::InvalidCall, "the download is not being held");
    }
    if (!impl.file_allowed(it->second.offer.clipboard, true)) {
        return Error(Status::PermissionDenied, "file transfer is disabled by the host");
    }
    it->second.accepted = true;
    return Error::success();
}

// ----- shared-folder browsing -----

bool QuicSession::take_file_list_request(uint32_t& request_id, std::string& directory,
                                         std::string* cursor, bool* show_hidden) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_list_requests.empty()) {
        return false;
    }
    request_id = impl_->file_list_requests.front().request_id;
    directory = std::move(impl_->file_list_requests.front().directory);
    if (cursor) *cursor = std::move(impl_->file_list_requests.front().cursor);
    if (show_hidden) *show_hidden = impl_->file_list_requests.front().show_hidden;
    impl_->file_list_requests.pop_front();
    return true;
}

Error QuicSession::send_file_list_result(const FileListResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_file_list_result is a server operation");
    }
    FileListResult answer = result;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the stream is not open");
        }
        if ((impl.effective_permissions() & kPermissionFileTransfer) == 0) {
            answer.entries.clear();
            answer.truncated = false;
            answer.next_cursor.clear();
            answer.code = FileCode::Refused;
            answer.reason = "file transfer was revoked";
        }
    }
    std::vector<uint8_t> message = make_file_list_result(answer);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid file list result");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_file_download_request(uint32_t& request_id, std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_download_requests.empty()) {
        return false;
    }
    request_id = impl_->file_download_requests.front().first;
    path = std::move(impl_->file_download_requests.front().second);
    impl_->file_download_requests.pop_front();
    return true;
}

Error QuicSession::send_file_download_result(const FileDownloadResult& result, bool clipboard) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_file_download_result is a server operation");
    }
    FileDownloadResult answer = result;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the stream is not open");
        }
        if (!impl.file_allowed(clipboard, true)) {
            answer.transfer_id = 0;
            answer.code = FileCode::Refused;
            answer.reason = "file transfer was revoked";
        }
    }
    std::vector<uint8_t> message = make_file_download_result(answer);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid file download result");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_file_list(uint32_t request_id, const std::string& directory,
                                     const std::string& cursor, bool show_hidden) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_file_list is a client operation");
    }
    std::vector<uint8_t> message = make_file_list_request(request_id, directory, cursor, show_hidden);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid file list request");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionFileTransfer) == 0) {
            return Error(Status::PermissionDenied,
                         "the host did not grant file transfer");
        }
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_file_list_result(FileListResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_list_results.empty()) {
        return false;
    }
    result = std::move(impl_->file_list_results.front());
    impl_->file_list_results.pop_front();
    return true;
}

Error QuicSession::request_file_download(uint32_t request_id, const std::string& path) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_file_download is a client operation");
    }
    std::vector<uint8_t> message = make_file_download_request(request_id, path);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid file download request");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if (!impl.file_allowed(path.starts_with(".chast-clipboard/"), false)) {
            return Error(Status::PermissionDenied,
                         "the host did not grant file transfer");
        }
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_file_download_result(FileDownloadResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_download_results.empty()) {
        return false;
    }
    result = std::move(impl_->file_download_results.front());
    impl_->file_download_results.pop_front();
    return true;
}

// ----- file management -----

bool QuicSession::take_file_op_request(uint32_t& request_id, FileOperation& op,
                                       std::string& path, std::string& new_name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_op_requests.empty()) {
        return false;
    }
    Impl::FileOpRequestData& request = impl_->file_op_requests.front();
    request_id = request.request_id;
    op = request.op;
    path = std::move(request.path);
    new_name = std::move(request.new_name);
    impl_->file_op_requests.pop_front();
    return true;
}

Error QuicSession::send_file_op_result(const FileOpResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_file_op_result is a server operation");
    }
    FileOpResult answer = result;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the stream is not open");
        }
        if ((impl.effective_permissions() & kPermissionFileTransfer) == 0) {
            answer.code = FileCode::Refused;
            answer.reason = "file transfer was revoked";
        }
    }
    std::vector<uint8_t> message = make_file_op_result(answer);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid file operation result");
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_file_op(uint32_t request_id, FileOperation op,
                                   const std::string& path,
                                   const std::string& new_name) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_file_op is a client operation");
    }
    std::vector<uint8_t> message = make_file_op_request(request_id, op, path, new_name);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid file operation request");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionFileTransfer) == 0) {
            return Error(Status::PermissionDenied,
                         "the host did not grant file transfer");
        }
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_file_op_result(FileOpResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_op_results.empty()) {
        return false;
    }
    result = std::move(impl_->file_op_results.front());
    impl_->file_op_results.pop_front();
    return true;
}

// ----- port forwarding -----

bool QuicSession::take_port_forward_request(PortForwardRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->port_forward_requests.empty()) {
        return false;
    }
    request = std::move(impl_->port_forward_requests.front());
    impl_->port_forward_requests.pop_front();
    return true;
}

Error QuicSession::send_port_forward_result(uint32_t forward_id, PortForwardCode code,
                                            const std::string& reason) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall,
                     "send_port_forward_result is a server operation");
    }
    if (forward_id == 0) {
        return Error(Status::InvalidCall, "the forward id is zero");
    }
    PortForwardResult result;
    result.forward_id = forward_id;
    result.code = code;
    result.reason = reason;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto pending = impl.pending_forward_targets.find(forward_id);
        if (pending == impl.pending_forward_targets.end()) {
            return Error(Status::InvalidCall, "no port forward request is waiting");
        }
        if (code == PortForwardCode::Ok) {
            if ((impl.effective_permissions() & kPermissionPortForward) == 0) {
                result.code = PortForwardCode::Refused;
                result.reason = "port forwarding was revoked";
            } else if (impl.active_forward_targets.size() >= kMaxPortForwardTargets) {
                result.code = PortForwardCode::Busy;
                result.reason = "too many forwarded targets";
            } else {
                impl.active_forward_targets[forward_id] = pending->second;
            }
        }
        impl.pending_forward_targets.erase(pending);
    }
    return impl.send_bytes(impl.control.load(), make_port_forward_result(result),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_port_forward_open(PortForwardOpen& open) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->port_forward_opens.empty()) {
        return false;
    }
    open = std::move(impl_->port_forward_opens.front());
    impl_->port_forward_opens.pop_front();
    return true;
}

Error QuicSession::accept_port_forward_connection(PortForwardHandle handle) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall,
                     "accept_port_forward_connection is a server operation");
    }
    uint32_t forward_id = 0;
    uint32_t connection_id = 0;
    bool finish_after_header = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        Impl::PortForwardEntry* entry = impl.port_forward_by_handle_locked(handle);
        if (!entry || !entry->server_side) {
            return Error(Status::InvalidCall, "no such port forward connection");
        }
        if (entry->accepted) {
            return Error(Status::InvalidCall, "the connection was already accepted");
        }
        if (entry->failed) {
            return Error(Status::Unavailable, entry->error);
        }
        if ((impl.effective_permissions() & kPermissionPortForward) == 0) {
            return Error(Status::PermissionDenied, "port forwarding was revoked");
        }
        forward_id = entry->forward_id;
        connection_id = entry->connection_id;
        finish_after_header = entry->send_finished;
    }
    HQUIC stream = nullptr;
    QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &Impl::stream_callback, &impl, &stream);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC StreamOpen(port forward)");
    }
    bool gone = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        Impl::PortForwardEntry* entry = impl.port_forward_by_handle_locked(handle);
        if (!entry || entry->failed) {
            gone = true;
        } else {
            entry->downlink = stream;
            entry->accepted = true;
            entry->receive_paused = false;
        }
    }
    if (gone) {
        impl.api->StreamClose(stream);
        return Error(Status::Unavailable, "the port forward connection ended while accepting");
    }
    QUIC_SEND_FLAGS flags = QUIC_SEND_FLAG_START;
    if (finish_after_header) {
        flags = static_cast<QUIC_SEND_FLAGS>(flags | QUIC_SEND_FLAG_FIN);
    }
    Error sent = impl.send_bytes(
        stream, make_port_forward_stream_header(forward_id, connection_id), flags);
    {
        // The viewer's bytes waited for the dial; let them through now. The
        // lookup and the API call share the lock, so a handle that a
        // SHUTDOWN_COMPLETE already forgot is never used.
        std::lock_guard<std::mutex> lock(impl.mutex);
        Impl::PortForwardEntry* entry = impl.port_forward_by_handle_locked(handle);
        if (entry && !entry->failed && entry->downlink == stream && entry->uplink) {
            impl.api->StreamReceiveSetEnabled(entry->uplink, TRUE);
        }
    }
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        Impl::PortForwardEntry* entry = impl.port_forward_by_handle_locked(handle);
        if (entry) {
            std::vector<HQUIC> aborts;
            impl.fail_port_forward_locked(*entry, sent.message(), aborts);
            impl.abort_streams_locked(aborts);
        }
        impl.cv.notify_all();
    }
    return sent;
}

Error QuicSession::reject_port_forward_connection(PortForwardHandle handle) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall,
                     "reject_port_forward_connection is a server operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.port_forward_connections.find(handle);
        if (it == impl.port_forward_connections.end() || !it->second.server_side) {
            return Error(Status::InvalidCall, "no such port forward connection");
        }
        if (it->second.accepted) {
            return Error(Status::InvalidCall, "the connection was already accepted");
        }
        if (it->second.uplink) {
            // Abort while the entry still owns the handle; SHUTDOWN_COMPLETE
            // cannot slip in and close it first.
            impl.api->StreamShutdown(it->second.uplink,
                                     QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        }
        impl.port_forward_connections.erase(it);
        impl.port_forward_opens.erase(
            std::remove_if(impl.port_forward_opens.begin(), impl.port_forward_opens.end(),
                           [handle](const PortForwardOpen& open) {
                               return open.handle == handle;
                           }),
            impl.port_forward_opens.end());
    }
    impl.cv.notify_all();
    return Error::success();
}

Error QuicSession::request_port_forward(uint32_t forward_id, const std::string& host,
                                        uint16_t port) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_port_forward is a client operation");
    }
    PortForwardRequest request;
    request.forward_id = forward_id;
    request.host = host;
    request.port = port;
    std::vector<uint8_t> message = make_port_forward_request(request);
    if (message.empty()) {
        return Error(Status::InvalidCall, "the target is not a valid host:port pair");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionPortForward) == 0) {
            return Error(Status::PermissionDenied, "the host did not grant port forwarding");
        }
        if (impl.client_pending_forwards.count(forward_id) != 0 ||
            impl.client_forward_targets.count(forward_id) != 0) {
            return Error(Status::InvalidCall, "that forward id is already registered");
        }
        if (impl.client_pending_forwards.size() + impl.client_forward_targets.size() >=
            kMaxPortForwardTargets) {
            return Error(Status::InvalidCall, "too many forwarded targets");
        }
        Impl::PortForwardTarget target;
        target.host = host;
        target.port = port;
        impl.client_pending_forwards.emplace(forward_id, std::move(target));
    }
    Error sent = impl.send_bytes(impl.control.load(), std::move(message),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.client_pending_forwards.erase(forward_id);
    }
    return sent;
}

bool QuicSession::take_port_forward_result(PortForwardResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->port_forward_results.empty()) {
        return false;
    }
    result = std::move(impl_->port_forward_results.front());
    impl_->port_forward_results.pop_front();
    return true;
}

Error QuicSession::open_port_forward_connection(uint32_t forward_id,
                                                uint32_t connection_id,
                                                PortForwardHandle& handle) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall,
                     "open_port_forward_connection is a client operation");
    }
    if (forward_id == 0 || connection_id == 0) {
        return Error(Status::InvalidCall, "a port forward id is zero");
    }
    PortForwardHandle created = 0;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionPortForward) == 0) {
            return Error(Status::PermissionDenied, "the host did not grant port forwarding");
        }
        if (impl.client_forward_targets.count(forward_id) == 0) {
            return Error(Status::InvalidCall, "the host did not approve that target");
        }
        if (impl.has_port_forward_entry_locked(forward_id, connection_id)) {
            return Error(Status::InvalidCall, "that connection id is already in use");
        }
        if (impl.port_forward_live_count_locked() >= kMaxPortForwardConnections) {
            impl.prune_retired_port_forwards_locked();
        }
        if (impl.port_forward_live_count_locked() >= kMaxPortForwardConnections) {
            return Error(Status::InvalidCall, "too many forwarded connections");
        }
        Impl::PortForwardEntry entry;
        entry.handle = impl.next_port_forward_handle++;
        entry.forward_id = forward_id;
        entry.connection_id = connection_id;
        created = entry.handle;
        impl.port_forward_connections.emplace(created, std::move(entry));
    }
    HQUIC stream = nullptr;
    QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &Impl::stream_callback, &impl, &stream);
    if (QUIC_FAILED(status)) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.port_forward_connections.erase(created);
        return quic_error(status, "QUIC StreamOpen(port forward)");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.port_forward_connections.find(created);
        if (it == impl.port_forward_connections.end()) {
            impl.api->StreamClose(stream);
            return Error(Status::Unavailable, "the port forward connection was closed");
        }
        it->second.uplink = stream;
    }
    Error sent = impl.send_bytes(
        stream, make_port_forward_stream_header(forward_id, connection_id),
        QUIC_SEND_FLAG_START);
    if (!sent.ok()) {
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.port_forward_connections.erase(created);
        }
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        impl.api->StreamClose(stream);
        return sent;
    }
    handle = created;
    return Error::success();
}

Error QuicSession::wait_port_forward_connection(PortForwardHandle handle,
                                                std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall,
                     "wait_port_forward_connection is a client operation");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    for (;;) {
        const auto it = impl.port_forward_connections.find(handle);
        if (it == impl.port_forward_connections.end()) {
            return Error(Status::InvalidCall, "the port forward connection is not open");
        }
        const Impl::PortForwardEntry& entry = it->second;
        if (entry.server_side) {
            return Error(Status::InvalidCall, "not a viewer connection");
        }
        if (entry.accepted) {
            return Error::success();
        }
        if (entry.failed) {
            return Error(Status::Unavailable, entry.error);
        }
        if (entry.receive_finished) {
            return Error(Status::Unavailable, "the host ended the port forward connection");
        }
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.shutdown_complete) {
            return Error(Status::Unavailable, "the connection closed");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(Status::WaitTimeout, "the host has not answered the dial yet");
        }
        impl.cv.wait_until(lock, deadline);
    }
}

Error QuicSession::send_port_forward_stop(uint32_t forward_id, PortForwardCode code,
                                          const std::string& reason) {
    Impl& impl = *impl_;
    if (forward_id == 0) {
        return Error(Status::InvalidCall, "the forward id is zero");
    }
    PortForwardStop stop;
    stop.forward_id = forward_id;
    stop.code = code;
    stop.reason = reason;
    std::vector<uint8_t> message = make_port_forward_stop(stop);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid port forward stop code");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const std::vector<HQUIC> aborts = impl.stop_port_forward_target_locked(forward_id);
        impl.abort_streams_locked(aborts);
    }
    impl.cv.notify_all();
    return impl.send_bytes(impl.control.load(), std::move(message),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_port_forward_stop(PortForwardStop& stop) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->port_forward_stops.empty()) {
        return false;
    }
    stop = std::move(impl_->port_forward_stops.front());
    impl_->port_forward_stops.pop_front();
    return true;
}

Error QuicSession::send_port_forward_data(PortForwardHandle handle, const uint8_t* data,
                                          size_t size, std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (!data || size == 0 || size > kMaxPortForwardChunkBytes) {
        return Error(Status::InvalidCall,
                     "a port forward chunk is between 1 byte and 64 KiB");
    }
    HQUIC stream = nullptr;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.port_forward_connections.find(handle);
        if (it == impl.port_forward_connections.end()) {
            return Error(Status::InvalidCall, "the port forward connection is not open");
        }
        Impl::PortForwardEntry& entry = it->second;
        if (entry.failed) {
            return Error(Status::Unavailable, entry.error);
        }
        if (entry.send_finished) {
            return Error(Status::Unavailable, "the sending direction is closed");
        }
        stream = Impl::port_forward_send_stream(entry);
        if (!stream) {
            return Error(Status::Unavailable,
                         "the host has not accepted the port forward connection");
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Error sent = impl.send_bytes_until(stream, std::vector<uint8_t>(data, data + size),
                                       QUIC_SEND_FLAG_NONE, deadline);
    if (!sent.ok() && sent.status() != Status::WaitTimeout) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.port_forward_connections.find(handle);
        if (it != impl.port_forward_connections.end()) {
            std::vector<HQUIC> aborts;
            impl.fail_port_forward_locked(it->second, sent.message(), aborts);
            impl.abort_streams_locked(aborts, stream);
        }
        impl.cv.notify_all();
    }
    return sent;
}

Error QuicSession::receive_port_forward_data(PortForwardHandle handle,
                                             std::vector<uint8_t>& data,
                                             std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    Error result = Error::success();
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto it = impl.port_forward_connections.find(handle);
            if (it == impl.port_forward_connections.end()) {
                result = Error(Status::InvalidCall,
                               "the port forward connection is not open");
                break;
            }
            Impl::PortForwardEntry& entry = it->second;
            if (!entry.received.empty()) {
                data = std::move(entry.received.front());
                entry.received.pop_front();
                entry.buffered_bytes -=
                    entry.buffered_bytes >= data.size() ? data.size() : entry.buffered_bytes;
                if (entry.receive_paused &&
                    entry.buffered_bytes <= kPortForwardReceiveHighWaterBytes / 2) {
                    entry.receive_paused = false;
                    if (HQUIC stream = Impl::port_forward_receive_stream(entry)) {
                        impl.api->StreamReceiveSetEnabled(stream, TRUE);
                    }
                }
                break;
            }
            if (entry.failed) {
                result = Error(Status::Unavailable, entry.error);
                break;
            }
            if (entry.receive_finished) {
                result = Error(Status::Unavailable,
                               "the peer ended the port forward connection");
                break;
            }
            if (impl.transport_failed) {
                result = Error(impl.transport_status, impl.transport_message);
                break;
            }
            if (impl.shutdown_complete) {
                result = Error(Status::Unavailable, "the connection closed");
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result = Error(Status::WaitTimeout, "no port forward data arrived in time");
                break;
            }
            impl.cv.wait_until(lock, deadline);
        }
    }
    return result;
}

Error QuicSession::finish_port_forward(PortForwardHandle handle) {
    Impl& impl = *impl_;
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.port_forward_connections.find(handle);
        if (it == impl.port_forward_connections.end()) {
            return Error(Status::InvalidCall, "the port forward connection is not open");
        }
        Impl::PortForwardEntry& entry = it->second;
        if (entry.send_finished) {
            return Error::success();
        }
        stream = Impl::port_forward_send_stream(entry);
        entry.send_finished = true;
        // The host may half-close before it accepted; accept_port_forward_
        // connection() then sends the header with FIN in one go.
        if (!stream) {
            return Error::success();
        }
    }
    Error sent = impl.send_stream_fin(stream);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.port_forward_connections.find(handle);
        if (it != impl.port_forward_connections.end()) {
            std::vector<HQUIC> aborts;
            impl.fail_port_forward_locked(it->second, sent.message(), aborts);
            impl.abort_streams_locked(aborts, stream);
        }
        impl.cv.notify_all();
    }
    return sent;
}

void QuicSession::close_port_forward(PortForwardHandle handle) {
    Impl& impl = *impl_;
    std::vector<HQUIC> streams;
    bool graceful = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.port_forward_connections.find(handle);
        if (it == impl.port_forward_connections.end()) {
            return;
        }
        Impl::PortForwardEntry& entry = it->second;
        if (entry.app_closed) {
            return;
        }
        // The relay only reaches this point after both directions finished
        // cleanly, and the last FIN may still be sitting in the send queue.
        // Aborting now would truncate it, so leave the streams to complete on
        // their own; SHUTDOWN_COMPLETE closes the handles.
        graceful = !entry.failed && entry.send_finished && entry.receive_finished;
        if (graceful) {
            entry.app_closed = true;
        } else {
            if (entry.uplink) {
                streams.push_back(entry.uplink);
            }
            if (entry.downlink && entry.downlink != entry.uplink) {
                streams.push_back(entry.downlink);
            }
            impl.port_forward_connections.erase(it);
            impl.port_forward_opens.erase(
                std::remove_if(impl.port_forward_opens.begin(),
                               impl.port_forward_opens.end(),
                               [handle](const PortForwardOpen& open) {
                                   return open.handle == handle;
                               }),
                impl.port_forward_opens.end());
            impl.abort_streams_locked(streams);
        }
    }
    for (HQUIC stream : streams) {
        impl.api->StreamClose(stream);
    }
    impl.cv.notify_all();
}

// ----- remote terminal -----

bool QuicSession::take_terminal_request(TerminalRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->terminal_requests.empty()) {
        return false;
    }
    request = std::move(impl_->terminal_requests.front());
    impl_->terminal_requests.pop_front();
    return true;
}

Error QuicSession::send_terminal_result(const TerminalResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_terminal_result is a server operation");
    }
    if (result.terminal_id == 0 || make_terminal_result(result).empty()) {
        return Error(Status::InvalidCall, "invalid terminal result");
    }
    TerminalResult answer = result;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto pending = impl.pending_terminal_targets.find(result.terminal_id);
        if (pending == impl.pending_terminal_targets.end()) {
            return Error(Status::InvalidCall, "no terminal request is waiting");
        }
        if (answer.code == TerminalCode::Ok) {
            if ((impl.effective_permissions() & kPermissionTerminal) == 0) {
                answer.code = TerminalCode::Refused;
                answer.reason = "terminal access was revoked";
                answer.service_id = 0;
            } else if (impl.active_terminal_targets.size() >= kMaxTerminals) {
                answer.code = TerminalCode::Busy;
                answer.reason = "too many terminals";
                answer.service_id = 0;
            } else {
                impl.active_terminal_targets[result.terminal_id] = pending->second;
                Impl::TerminalEntry entry;
                entry.terminal_id = result.terminal_id;
                entry.server_side = true;
                entry.columns = pending->second.columns;
                entry.rows = pending->second.rows;
                impl.terminal_entries.emplace(result.terminal_id, std::move(entry));
            }
        }
        impl.pending_terminal_targets.erase(pending);
    }
    return impl.send_bytes(impl.control.load(), make_terminal_result(answer),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::request_terminal(uint32_t terminal_id, uint16_t columns, uint16_t rows,
                                    uint8_t flags, uint32_t service_id) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_terminal is a client operation");
    }
    TerminalRequest request;
    request.terminal_id = terminal_id;
    request.columns = columns;
    request.rows = rows;
    request.flags = flags;
    request.service_id = service_id;
    std::vector<uint8_t> message = make_terminal_request(request);
    if (message.empty()) {
        return Error(Status::InvalidCall, "the terminal id, size or flags are out of range");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionTerminal) == 0) {
            return Error(Status::PermissionDenied, "the host did not grant terminals");
        }
        if (impl.client_pending_terminals.count(terminal_id) != 0 ||
            impl.client_terminal_targets.count(terminal_id) != 0) {
            return Error(Status::InvalidCall, "that terminal id is already in use");
        }
        if (impl.client_pending_terminals.size() + impl.client_terminal_targets.size() >=
            kMaxTerminals) {
            return Error(Status::InvalidCall, "too many terminals");
        }
        TerminalSize size;
        size.terminal_id = terminal_id;
        size.columns = columns;
        size.rows = rows;
        impl.client_pending_terminals.emplace(terminal_id, size);
    }
    Error sent = impl.send_bytes(impl.control.load(), std::move(message),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.client_pending_terminals.erase(terminal_id);
    }
    return sent;
}

bool QuicSession::take_terminal_result(TerminalResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->terminal_results.empty()) {
        return false;
    }
    result = std::move(impl_->terminal_results.front());
    impl_->terminal_results.pop_front();
    return true;
}

Error QuicSession::open_terminal_input(uint32_t terminal_id) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "open_terminal_input is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end() || it->second.server_side) {
            return Error(Status::InvalidCall, "no such terminal");
        }
        const Impl::TerminalEntry& entry = it->second;
        if (entry.failed) {
            return Error(Status::Unavailable, entry.error);
        }
        if (entry.send_finished) {
            return Error(Status::Unavailable, "the input direction is closed");
        }
        if (entry.uplink) {
            return Error(Status::InvalidCall, "the terminal input stream is already open");
        }
        if ((impl.effective_permissions() & kPermissionTerminal) == 0) {
            return Error(Status::PermissionDenied, "terminal access was revoked");
        }
    }
    HQUIC stream = nullptr;
    QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &Impl::stream_callback, &impl, &stream);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC StreamOpen(terminal input)");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end() || it->second.failed ||
            it->second.uplink) {
            impl.api->StreamClose(stream);
            return Error(Status::Unavailable, "the terminal ended while opening its input");
        }
        it->second.uplink = stream;
    }
    Error sent = impl.send_bytes(stream, make_terminal_stream_header(terminal_id),
                                 QUIC_SEND_FLAG_START);
    if (!sent.ok()) {
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            const auto it = impl.terminal_entries.find(terminal_id);
            if (it != impl.terminal_entries.end() && it->second.uplink == stream) {
                it->second.uplink = nullptr;
            }
        }
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        impl.api->StreamClose(stream);
        return sent;
    }
    impl.cv.notify_all();
    return Error::success();
}

Error QuicSession::send_terminal_input(uint32_t terminal_id, const uint8_t* data,
                                       size_t size, std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (!data || size == 0 || size > kMaxTerminalChunkBytes) {
        return Error(Status::InvalidCall, "a terminal chunk is between 1 byte and 64 KiB");
    }
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end() || it->second.server_side) {
            return Error(Status::InvalidCall, "no such terminal");
        }
        Impl::TerminalEntry& entry = it->second;
        if (entry.failed) {
            return Error(Status::Unavailable, entry.error);
        }
        if (entry.send_finished) {
            return Error(Status::Unavailable, "the input direction is closed");
        }
        stream = entry.uplink;
        if (!stream) {
            return Error(Status::Unavailable, "the input stream is not open");
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Error sent = impl.send_bytes_until(stream, std::vector<uint8_t>(data, data + size),
                                       QUIC_SEND_FLAG_NONE, deadline);
    if (!sent.ok() && sent.status() != Status::WaitTimeout) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it != impl.terminal_entries.end()) {
            std::vector<HQUIC> aborts;
            impl.fail_terminal_locked(it->second, sent.message(), aborts);
            impl.abort_streams_locked(aborts, stream);
        }
        impl.cv.notify_all();
    }
    return sent;
}

Error QuicSession::finish_terminal_input(uint32_t terminal_id) {
    Impl& impl = *impl_;
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end() || it->second.server_side) {
            return Error(Status::InvalidCall, "no such terminal");
        }
        Impl::TerminalEntry& entry = it->second;
        if (entry.send_finished) {
            return Error::success();
        }
        entry.send_finished = true;
        stream = entry.uplink;
        if (!stream) {
            return Error::success();
        }
    }
    Error sent = impl.send_stream_fin(stream);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it != impl.terminal_entries.end()) {
            std::vector<HQUIC> aborts;
            impl.fail_terminal_locked(it->second, sent.message(), aborts);
            impl.abort_streams_locked(aborts, stream);
        }
        impl.cv.notify_all();
    }
    return sent;
}

Error QuicSession::send_terminal_resize(uint32_t terminal_id, uint16_t columns,
                                        uint16_t rows) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "send_terminal_resize is a client operation");
    }
    std::vector<uint8_t> message = make_terminal_resize(terminal_id, columns, rows);
    if (message.empty()) {
        return Error(Status::InvalidCall, "the terminal id or size is out of range");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionTerminal) == 0) {
            return Error(Status::PermissionDenied, "terminal access was revoked");
        }
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_terminal_output(uint32_t terminal_id, std::vector<uint8_t>& data,
                                       bool& ended) {
    Impl& impl = *impl_;
    bool popped = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        ended = false;
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end()) {
            ended = true;
            return false;
        }
        Impl::TerminalEntry& entry = it->second;
        if (!entry.received.empty()) {
            data = std::move(entry.received.front());
            entry.received.pop_front();
            entry.buffered_bytes -=
                entry.buffered_bytes >= data.size() ? data.size() : entry.buffered_bytes;
            if (entry.receive_paused &&
                entry.buffered_bytes <= kTerminalReceiveHighWaterBytes / 2) {
                entry.receive_paused = false;
                if (HQUIC stream = Impl::terminal_receive_stream(entry)) {
                    impl.api->StreamReceiveSetEnabled(stream, TRUE);
                }
            }
            popped = true;
        }
        if (!popped) {
            // A stop alone is not the end: output already on the wire still
            // has to arrive, so only the stream's own end (or a failure)
            // reports ended.
            ended = entry.receive_finished || entry.failed;
        }
        if (entry.stopped && entry.receive_finished && entry.received.empty()) {
            // The host ended the terminal and everything it wrote has been
            // handed over. The entry goes once QUIC released both handles.
            ended = true;
            if (!entry.uplink && !entry.downlink) {
                impl.terminal_entries.erase(it);
            }
        }
    }
    return popped;
}

bool QuicSession::take_terminal_resize(TerminalSize& size) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->terminal_resizes.empty()) {
        return false;
    }
    size = impl_->terminal_resizes.front();
    impl_->terminal_resizes.pop_front();
    return true;
}

bool QuicSession::take_terminal_input(uint32_t terminal_id, std::vector<uint8_t>& data,
                                      bool& ended) {
    Impl& impl = *impl_;
    bool popped = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        ended = false;
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end()) {
            ended = true;
            return false;
        }
        Impl::TerminalEntry& entry = it->second;
        if (!entry.received.empty()) {
            data = std::move(entry.received.front());
            entry.received.pop_front();
            entry.buffered_bytes -=
                entry.buffered_bytes >= data.size() ? data.size() : entry.buffered_bytes;
            if (entry.receive_paused &&
                entry.buffered_bytes <= kTerminalReceiveHighWaterBytes / 2) {
                entry.receive_paused = false;
                if (HQUIC stream = Impl::terminal_receive_stream(entry)) {
                    impl.api->StreamReceiveSetEnabled(stream, TRUE);
                }
            }
            popped = true;
        } else {
            ended = entry.receive_finished || entry.failed;
        }
    }
    return popped;
}

Error QuicSession::send_terminal_output(uint32_t terminal_id, const uint8_t* data,
                                        size_t size, std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (!data || size == 0 || size > kMaxTerminalChunkBytes) {
        return Error(Status::InvalidCall, "a terminal chunk is between 1 byte and 64 KiB");
    }
    HQUIC stream = nullptr;
    bool need_open = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end() || !it->second.server_side) {
            return Error(Status::InvalidCall, "no such terminal");
        }
        Impl::TerminalEntry& entry = it->second;
        if (entry.failed) {
            return Error(Status::Unavailable, entry.error);
        }
        if (entry.send_finished) {
            return Error(Status::Unavailable, "the output direction is closed");
        }
        if ((impl.effective_permissions() & kPermissionTerminal) == 0) {
            return Error(Status::PermissionDenied, "terminal access was revoked");
        }
        stream = entry.downlink;
        need_open = stream == nullptr;
    }
    if (need_open) {
        HQUIC opened = nullptr;
        QUIC_STATUS status = impl.api->StreamOpen(
            impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            &Impl::stream_callback, &impl, &opened);
        if (QUIC_FAILED(status)) {
            return quic_error(status, "QUIC StreamOpen(terminal output)");
        }
        bool attached = false;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            const auto it = impl.terminal_entries.find(terminal_id);
            if (it != impl.terminal_entries.end() && !it->second.failed &&
                it->second.downlink == nullptr) {
                it->second.downlink = opened;
                attached = true;
            }
        }
        if (!attached) {
            impl.api->StreamClose(opened);
            return Error(Status::Unavailable, "the terminal ended while opening its output");
        }
        Error sent = impl.send_bytes(opened, make_terminal_stream_header(terminal_id),
                                     QUIC_SEND_FLAG_START);
        if (!sent.ok()) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            const auto it = impl.terminal_entries.find(terminal_id);
            if (it != impl.terminal_entries.end()) {
                std::vector<HQUIC> aborts;
                impl.fail_terminal_locked(it->second, sent.message(), aborts);
                impl.abort_streams_locked(aborts);
            }
            impl.cv.notify_all();
            return sent;
        }
        stream = opened;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Error sent = impl.send_bytes_until(stream, std::vector<uint8_t>(data, data + size),
                                       QUIC_SEND_FLAG_NONE, deadline);
    if (!sent.ok() && sent.status() != Status::WaitTimeout) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it != impl.terminal_entries.end()) {
            std::vector<HQUIC> aborts;
            impl.fail_terminal_locked(it->second, sent.message(), aborts);
            impl.abort_streams_locked(aborts, stream);
        }
        impl.cv.notify_all();
    }
    return sent;
}

Error QuicSession::finish_terminal_output(uint32_t terminal_id) {
    Impl& impl = *impl_;
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it == impl.terminal_entries.end() || !it->second.server_side) {
            return Error(Status::InvalidCall, "no such terminal");
        }
        Impl::TerminalEntry& entry = it->second;
        if (entry.send_finished) {
            return Error::success();
        }
        entry.send_finished = true;
        stream = entry.downlink;
        if (!stream) {
            // The console never printed; the stop message carries the outcome.
            return Error::success();
        }
    }
    Error sent = impl.send_stream_fin(stream);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        const auto it = impl.terminal_entries.find(terminal_id);
        if (it != impl.terminal_entries.end()) {
            std::vector<HQUIC> aborts;
            impl.fail_terminal_locked(it->second, sent.message(), aborts);
            impl.abort_streams_locked(aborts, stream);
        }
        impl.cv.notify_all();
    }
    return sent;
}

Error QuicSession::send_terminal_stop(uint32_t terminal_id, TerminalCode code,
                                      const std::string& reason) {
    Impl& impl = *impl_;
    if (terminal_id == 0) {
        return Error(Status::InvalidCall, "the terminal id is zero");
    }
    TerminalStop stop;
    stop.terminal_id = terminal_id;
    stop.code = code;
    stop.reason = reason;
    std::vector<uint8_t> message = make_terminal_stop(stop);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid terminal stop code");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        // A direction this side already finished cleanly keeps its FIN: a
        // reset would tell the peer the transfer failed and make it drop bytes
        // still queued on its side.
        const std::vector<HQUIC> aborts = impl.drop_terminal_locked(terminal_id, true);
        impl.abort_streams_locked(aborts);
    }
    impl.cv.notify_all();
    return impl.send_bytes(impl.control.load(), std::move(message),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_terminal_stop(TerminalStop& stop) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->terminal_stops.empty()) {
        return false;
    }
    stop = std::move(impl_->terminal_stops.front());
    impl_->terminal_stops.pop_front();
    return true;
}

void QuicSession::close_terminal(uint32_t terminal_id) {
    Impl& impl = *impl_;
    std::vector<HQUIC> streams;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        streams = impl.drop_terminal_locked(terminal_id);
        impl.abort_streams_locked(streams);
    }
    for (HQUIC stream : streams) {
        impl.api->StreamClose(stream);
    }
    impl.cv.notify_all();
}

// ----- camera -----

bool QuicSession::take_camera_list_request() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool ready = impl_->camera_list_request_ready;
    impl_->camera_list_request_ready = false;
    return ready;
}

Error QuicSession::send_camera_list(const CameraList& list) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_camera_list is a server operation");
    }
    std::vector<uint8_t> message = make_camera_list(list);
    if (message.empty()) {
        return Error(Status::InvalidCall, "invalid camera list");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
    }
    return impl.send_bytes(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_camera_start(uint16_t& camera_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->camera_starts.empty()) {
        return false;
    }
    camera_id = impl_->camera_starts.front();
    impl_->camera_starts.pop_front();
    return true;
}

Error QuicSession::send_camera_result(const CameraResult& result) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_camera_result is a server operation");
    }
    if (result.camera_id == 0 || make_camera_result(result).empty()) {
        return Error(Status::InvalidCall, "invalid camera result");
    }
    CameraResult answer = result;
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the stream is not open");
        }
        if (result.camera_id != impl.server_pending_camera) {
            return Error(Status::InvalidCall, "no camera request is waiting");
        }
        impl.server_pending_camera = 0;
        if ((impl.effective_permissions() & kPermissionCamera) == 0) {
            answer.code = CameraCode::Refused;
            answer.reason = "camera access was revoked";
        } else if (answer.code == CameraCode::Ok && impl.camera.load() != nullptr &&
                   !impl.camera_finished) {
            answer.code = CameraCode::Busy;
            answer.reason = "another camera is already streaming";
        }
    }
    if (answer.code == CameraCode::Ok) {
        // The header names the id and goes first on the new stream, so the
        // viewer can classify it before the CameraResult reaches it.
        QUIC_STATUS status = impl.api->StreamOpen(
            impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            &Impl::stream_callback, &impl, &stream);
        if (QUIC_FAILED(status)) {
            return quic_error(status, "QUIC StreamOpen(camera)");
        }
        Error sent = impl.send_bytes(stream, make_camera_stream_header(result.camera_id),
                                     QUIC_SEND_FLAG_START);
        if (!sent.ok()) {
            impl.api->StreamClose(stream);
            return sent;
        }
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.camera = stream;
            impl.camera_finished = false;
            impl.server_camera_id = result.camera_id;
            impl.next_camera_seq = 0;
        }
    }
    Error sent = impl.send_bytes(impl.control.load(), make_camera_result(answer),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok() && stream) {
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            if (impl.camera.load() == stream) {
                impl.camera = nullptr;
                impl.camera_finished = true;
                impl.server_camera_id = 0;
            }
            impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        }
    }
    impl.cv.notify_all();
    return sent;
}

Error QuicSession::send_camera_frame(FrameHeader header, const uint8_t* payload,
                                     size_t size) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "send_camera_frame is a server operation");
    }
    if (!payload || size == 0 || size > kMaxFramePayload) {
        return Error(Status::InvalidCall, "invalid frame payload size");
    }
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.announced) {
            return Error(Status::InvalidCall, "the camera stream is not open");
        }
        if ((impl.effective_permissions() & kPermissionCamera) == 0) {
            return Error(Status::PermissionDenied, "camera access was revoked");
        }
        if (!(impl.decoding_codecs & video_codec_bit(header.codec)) ||
            (header.chroma == VideoChroma::Yuv444 && !(impl.decoding_444 & video_codec_bit(header.codec)))) {
            return Error(Status::Unsupported, "the viewer cannot decode this camera format");
        }
        if (impl.camera_finished) {
            return Error(Status::Unavailable, "the camera stream is finished");
        }
        stream = impl.camera.load();
    }
    if (!stream) {
        return Error(Status::Unavailable, "the camera stream is not open");
    }
    header.seq = impl.next_camera_seq.fetch_add(1);
    header.payload_size = static_cast<uint32_t>(size);
    header.payload_crc32 = crc32(payload, size);
    std::vector<uint8_t> bytes(kFrameHeaderSize + size);
    encode_frame_header(header, bytes.data());
    std::memcpy(bytes.data() + kFrameHeaderSize, payload, size);
    // Never waits: a stalled queue drops the frame instead of stalling the
    // camera capture loop, and the caller can send a keyframe next time.
    return impl.send_bytes_impl(stream, std::move(bytes), QUIC_SEND_FLAG_NONE, false);
}

Error QuicSession::finish_camera() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server) {
        return Error(Status::InvalidCall, "finish_camera is a server operation");
    }
    HQUIC stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.camera_finished) {
            return Error::success();
        }
        stream = impl.camera.load();
        if (!stream) {
            return Error::success();
        }
        impl.camera_finished = true;
        impl.server_camera_id = 0;
        // Shut down under the lock so a teardown that forgets the handle
        // cannot close it between the load and this call.
        impl.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    }
    impl.cv.notify_all();
    return Error::success();
}

bool QuicSession::camera_open() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->camera.load() != nullptr && !impl_->camera_finished;
}

bool QuicSession::take_camera_stop(uint16_t& camera_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->camera_stops.empty()) {
        return false;
    }
    camera_id = impl_->camera_stops.front();
    impl_->camera_stops.pop_front();
    return true;
}

Error QuicSession::request_camera_list() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_camera_list is a client operation");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionCamera) == 0) {
            return Error(Status::PermissionDenied, "the host did not grant cameras");
        }
    }
    return impl.send_bytes(impl.control.load(), make_camera_list_request(),
                           QUIC_SEND_FLAG_NONE);
}

bool QuicSession::take_camera_list(CameraList& list) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->camera_lists_queue.empty()) {
        return false;
    }
    // The newest answer supersedes whatever arrived before it.
    list = std::move(impl_->camera_lists_queue.back());
    impl_->camera_lists_queue.clear();
    return true;
}

uint64_t QuicSession::camera_lists() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->camera_lists_seen;
}

Error QuicSession::request_camera_start(uint16_t camera_id) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_camera_start is a client operation");
    }
    std::vector<uint8_t> message = make_camera_start(camera_id);
    if (message.empty()) {
        return Error(Status::InvalidCall, "the camera id is zero");
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        if ((impl.effective_permissions() & kPermissionCamera) == 0) {
            return Error(Status::PermissionDenied, "the host did not grant cameras");
        }
        if (impl.camera_finished) {
            // The previous camera ended; its ids and buffered frames belong to
            // it and must not block or leak into the new one.
            impl.client_camera_id = 0;
            impl.client_pending_camera = 0;
            impl.camera_received.clear();
            impl.camera_parser.reset();
        }
        if (impl.client_pending_camera != 0 || impl.client_camera_id != 0 ||
            (impl.camera.load() != nullptr && !impl.camera_finished)) {
            return Error(Status::InvalidCall, "a camera is already running");
        }
        impl.client_pending_camera = camera_id;
    }
    Error sent = impl.send_bytes(impl.control.load(), std::move(message),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.client_pending_camera == camera_id) {
            impl.client_pending_camera = 0;
        }
    }
    return sent;
}

bool QuicSession::take_camera_result(CameraResult& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->camera_results.empty()) {
        return false;
    }
    result = std::move(impl_->camera_results.front());
    impl_->camera_results.pop_front();
    return true;
}

Error QuicSession::receive_camera_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                                        std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "receive_camera_frame is a client operation");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    for (;;) {
        while (!impl.camera_received.empty()) {
            std::vector<uint8_t> chunk = impl.camera_received.take_front();
            std::string error;
            if (!impl.camera_parser.push(chunk.data(), chunk.size(), &error)) {
                return Error(Status::CaptureFailed, "camera stream: " + error);
            }
        }
        if (!impl.camera_finished && !impl.shutdown_complete) {
            impl.resume_media_locked(impl.camera.load(), impl.camera_received,
                                     kCameraReceiveHighWaterBytes);
        }

        const uint8_t* frame_payload = nullptr;
        std::string parse_error;
        switch (impl.camera_parser.next(header, &frame_payload, &parse_error)) {
            case FrameParser::Status::Frame: {
                payload.assign(frame_payload, frame_payload + header.payload_size);
                impl.camera_parser.consume();
                return Error::success();
            }
            case FrameParser::Status::Error:
                return Error(Status::CaptureFailed, "camera stream: " + parse_error);
            case FrameParser::Status::Incomplete:
                break;
        }

        if (impl.camera_finished) {
            if (impl.camera_parser.buffered() > 0) {
                return Error(Status::CaptureFailed, "camera stream ended mid-frame");
            }
            return Error(Status::Unavailable, "the host finished the camera stream");
        }
        if (impl.transport_failed) {
            return Error(impl.transport_status, impl.transport_message);
        }
        if (impl.shutdown_complete) {
            return Error(Status::Unavailable, "the connection closed");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(Status::WaitTimeout, "no camera frame arrived in time");
        }
        impl.cv.wait_until(lock, deadline);
    }
}

Error QuicSession::request_camera_stop() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_camera_stop is a client operation");
    }
    uint16_t camera_id = 0;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.hello_complete) {
            return Error(Status::InvalidCall, "the handshake has not completed");
        }
        camera_id = impl.client_camera_id != 0 ? impl.client_camera_id
                                               : impl.client_pending_camera;
        if (camera_id == 0) {
            return Error::success();
        }
        impl.client_pending_camera = 0;
        impl.client_camera_id = 0;
        // The viewer asked to stop: whichever frames were buffered are over
        // and the next start belongs to a fresh timeline.
        impl.camera_finished = true;
        impl.camera_received.clear();
        impl.camera_parser.reset();
    }
    impl.cv.notify_all();
    return impl.send_bytes(impl.control.load(), make_camera_stop(camera_id),
                           QUIC_SEND_FLAG_NONE);
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

Error QuicSession::send_display_cursor(const DisplayCursorUpdate& update) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Server || !impl.desktop_subscription_ready())
        return Error(Status::InvalidCall, "display cursor requires an accepted desktop viewer");
    auto message = make_display_cursor_update(update);
    if (message.empty()) return Error(Status::InvalidCall, "invalid display cursor");
    return impl.send_bytes_impl(impl.control.load(), std::move(message), QUIC_SEND_FLAG_NONE, false);
}

bool QuicSession::take_display_cursor_update(DisplayCursorUpdate& update) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->display_cursor_updates.empty()) return false;
    auto it = impl_->display_cursor_updates.begin();
    update.stream_id = it->first;
    update.cursor = std::move(it->second);
    impl_->display_cursor_updates.erase(it);
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

bool QuicSession::audio_datagram() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->peer_info.audio_datagram;
}

Error QuicSession::async_error() const {
    return impl_->recorded_error();
}

}  // namespace chast
