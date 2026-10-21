// MsQuic transport and session state for chast/protocol.h.
//
// Threading model: MsQuic delivers listener, connection and stream events on
// its own worker threads; this file records them under Impl::mutex. All public
// operations run on the application thread and wait on condition variables.
// The only object shared with the transport besides handles is the receive
// queue (protected) and PendingSend contexts (owned by MsQuic until
// SEND_COMPLETE, then deleted).
//
// The server certificate is a CNG RSA key in the user's key store plus a
// self-signed certificate created at startup. The client turns validation off
// for localhost development; the rendezvous phase will replace this with a
// pinned peer identity.

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
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace chast {

namespace {

constexpr size_t kMaxInFlightBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxInFlightSends = 4096;
constexpr uint64_t kKeepAliveMs = 5000;
constexpr wchar_t kKeyContainer[] = L"chast-quic-key";

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

// Creates the server credential. The RSA key lives in the user's CNG key store
// under a fixed container name so restarts reuse it; the certificate itself is
// rebuilt each run (it is only trusted because the client skips validation).
PCCERT_CONTEXT create_self_signed_certificate(std::string* error) {
    NCRYPT_PROV_HANDLE provider = 0;
    SECURITY_STATUS sec = NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0);
    if (FAILED(sec)) {
        *error = "NCryptOpenStorageProvider failed: " + sec_status_hex(sec);
        return nullptr;
    }
    NCRYPT_KEY_HANDLE key = 0;
    sec = NCryptOpenKey(provider, &key, kKeyContainer, 0, NCRYPT_SILENT_FLAG);
    if (sec == NTE_BAD_KEYSET) {
        sec = NCryptCreatePersistedKey(provider, &key, NCRYPT_RSA_ALGORITHM, kKeyContainer, 0, 0);
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
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost", CERT_X500_NAME_STR, nullptr,
                        nullptr, &subject_size, nullptr)) {
        *error = "CertStrToNameW failed";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }
    std::vector<BYTE> subject(subject_size);
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost", CERT_X500_NAME_STR, nullptr,
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
    key_info.dwFlags = NCRYPT_SILENT_FLAG;
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

    const QUIC_API_TABLE* api = nullptr;
    HQUIC registration = nullptr;
    HQUIC configuration = nullptr;
    HQUIC listener = nullptr;
    PCCERT_CONTEXT certificate = nullptr;
    std::atomic<HQUIC> connection{nullptr};
    std::atomic<HQUIC> control{nullptr};
    std::atomic<HQUIC> video{nullptr};

    mutable std::mutex mutex;
    std::condition_variable cv;
    bool connected = false;
    bool shutdown_complete = false;
    bool peer_closed = false;
    bool hello_ready = false;
    bool hello_ack_ready = false;
    bool video_finished = false;
    bool control_finished = false;
    bool video_shutdown_complete = false;
    bool control_shutdown_complete = false;
    bool stats_ready = false;
    bool bye_ready = false;
    bool keyframe_requested = false;
    bool announced = false;
    bool transport_failed = false;
    Status transport_status = Status::Ok;
    std::string transport_message;
    ServerInfo peer_info;
    ClientStats peer_stats;
    std::deque<std::vector<uint8_t>> received;
    ControlParser control_parser;
    FrameParser frame_parser;
    // Server only, when QuicServerConfig::allow_input is set.
    std::unique_ptr<InputInjector> injector;
    std::deque<InputEvent> input_events;

    std::atomic<uint64_t> next_seq{0};
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
            // refused before the handshake continues.
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
                    HQUIC expected = nullptr;
                    accepted = video.compare_exchange_strong(expected, stream);
                }
                if (!accepted) {
                    api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                }
                cv.notify_all();
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
                    std::deque<std::vector<uint8_t>> chunks;
                    for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
                        const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
                        chunks.emplace_back(buffer.Buffer, buffer.Buffer + buffer.Length);
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        for (std::vector<uint8_t>& chunk : chunks) {
                            received.push_back(std::move(chunk));
                        }
                    }
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
                    } else if (stream == control.load()) {
                        control_finished = true;
                    }
                }
                cv.notify_all();
                break;
            }
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
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
                if (!parse_hello(payload, &error)) {
                    note_transport_failure("Hello: " + error);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    hello_ready = true;
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

    Error send_bytes(HQUIC stream, std::vector<uint8_t> bytes, QUIC_SEND_FLAGS flags) {
        if (!stream) {
            return Error(Status::Unavailable, "no stream is open");
        }
        if (bytes.empty()) {
            return Error(Status::InvalidCall, "refusing to send an empty message");
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
            return quic_error(status, "QUIC StreamSend");
        }
        return Error::success();
    }

    bool wait_sends(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(send_mutex);
        return send_cv.wait_for(lock, timeout, [this] {
            return in_flight_sends == 0 || transport_down.load();
        });
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
                api->ConnectionShutdown(connection_handle, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(2000),
                            [this] { return shutdown_complete; });
            }
        }
        wait_sends(std::chrono::milliseconds(1000));

        // StreamClose is the only release for a stream handle, whether or not
        // its SHUTDOWN_COMPLETE was observed; closing before that implicitly
        // aborts, which is what teardown wants.
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

    impl.certificate = create_self_signed_certificate(error);
    if (!impl.certificate) {
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
    // The server opens one unidirectional video stream.
    settings.PeerUnidiStreamCount = 1;
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

    QUIC_CREDENTIAL_CONFIG credential{};
    credential.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credential.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (config.skip_certificate_validation) {
        credential.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
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
    std::unique_lock<std::mutex> lock(impl.mutex);
    impl.cv.wait_for(lock, timeout, [&impl] {
        return impl.hello_ready || impl.transport_failed || impl.shutdown_complete ||
               impl.peer_closed;
    });
    if (impl.transport_failed) {
        return Error(impl.transport_status, impl.transport_message);
    }
    if (impl.hello_ready) {
        return Error::success();
    }
    if (impl.shutdown_complete || impl.peer_closed) {
        return Error(Status::Unavailable, "client left before the handshake");
    }
    return Error(Status::WaitTimeout, "no client connected in time");
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
    // The gate belongs to the server; never let a caller announce input the
    // session will not inject.
    ack.input_enabled = impl.server_config.allow_input;
    Error sent = impl.send_bytes(impl.control.load(), make_hello_ack(ack),
                                 QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        return sent;
    }

    HQUIC video = nullptr;
    QUIC_STATUS status = impl.api->StreamOpen(
        impl.connection.load(), QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &Impl::stream_callback, &impl, &video);
    if (QUIC_FAILED(status)) {
        return quic_error(status, "QUIC StreamOpen(video)");
    }
    impl.video = video;
    status = impl.api->StreamStart(video, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        impl.video = nullptr;
        impl.api->StreamClose(video);
        return quic_error(status, "QUIC StreamStart(video)");
    }
    impl.announced = true;
    return Error::success();
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

std::vector<InputEvent> QuicSession::take_input_events() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<InputEvent> events(impl_->input_events.begin(), impl_->input_events.end());
    impl_->input_events.clear();
    return events;
}

InputStats QuicSession::input_stats() const {
    return impl_->injector ? impl_->injector->stats() : InputStats{};
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

Error QuicSession::handshake(ServerInfo& info, std::chrono::milliseconds timeout) {
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

    Error sent = impl.send_bytes(control, make_hello(), QUIC_SEND_FLAG_NONE);
    if (!sent.ok()) {
        return sent;
    }
    std::unique_lock<std::mutex> lock(impl.mutex);
    impl.cv.wait_for(lock, timeout, [&impl] {
        return impl.hello_ack_ready || impl.transport_failed || impl.shutdown_complete ||
               impl.peer_closed;
    });
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

Error QuicSession::request_keyframe() {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "request_keyframe is a client operation");
    }
    return impl.send_bytes(impl.control.load(), make_control(ControlType::KeyframeRequest, {}),
                           QUIC_SEND_FLAG_NONE);
}

Error QuicSession::send_input(const InputEvent& event) {
    Impl& impl = *impl_;
    if (impl.role != Impl::Role::Client) {
        return Error(Status::InvalidCall, "send_input is a client operation");
    }
    return impl.send_bytes(impl.control.load(), make_input_event(event), QUIC_SEND_FLAG_NONE);
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

Error QuicSession::async_error() const {
    return impl_->recorded_error();
}

}  // namespace chast
