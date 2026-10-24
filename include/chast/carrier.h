#pragma once

// TCP/WebSocket carrier for QUIC datagrams.
//
// QUIC needs UDP, which some networks block outright. A carrier keeps the
// existing session untouched: each TCP (or WebSocket) connection carries one
// QUIC connection's datagrams as length-prefixed messages, and the host opens
// a fresh UDP socket towards its own QUIC listener. TLS, fingerprint pinning,
// tokens, approval and every protocol message still run end to end between
// the two chast endpoints; the carrier only decides how the bytes travel.
//
// The host listens on QUIC port + 1 and accepts both raw TCP and WebSocket
// upgrades on that one port. A viewer bridge exposes the carrier as a
// loopback UDP endpoint, which is what MsQuic dials.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace chast {

enum class CarrierTransport {
    kTcp,
    kWebSocket,
};

const char* carrier_transport_name(CarrierTransport transport);

// Raw TCP connections and WebSocket upgrades share the listen port; the first
// bytes select the framing.
struct CarrierServerConfig {
    // Address the public listener binds to, normally the host's bind_address.
    std::string bind_address = "0.0.0.0";
    // QUIC port + 1, or any free TCP port. 0 asks the OS (tests).
    uint16_t port = 0;
    // The QUIC listener the carrier feeds. Loopback unless the host binds one
    // specific address.
    std::string quic_address = "127.0.0.1";
    uint16_t quic_port = 0;
    size_t max_connections = 16;
    // Called from connection threads; must not block.
    std::function<void(const std::string&)> log;
};

struct CarrierServerStats {
    uint64_t connections = 0;
    uint64_t active = 0;
    uint64_t rejected = 0;
    uint64_t datagrams_to_quic = 0;
    uint64_t datagrams_from_quic = 0;
    uint64_t bytes_to_quic = 0;
    uint64_t bytes_from_quic = 0;
};

class CarrierServer {
public:
    // Binds and starts accepting before returning. Null with `error` when the
    // port or address is unusable; the caller may continue QUIC-only.
    static std::unique_ptr<CarrierServer> create(const CarrierServerConfig& config,
                                                 std::string* error);
    ~CarrierServer();
    CarrierServer(const CarrierServer&) = delete;
    CarrierServer& operator=(const CarrierServer&) = delete;

    // The bound port (useful when config.port was 0).
    uint16_t port() const;
    CarrierServerStats stats() const;
    // Stops accepting, closes every connection and joins the threads. Safe to
    // call twice.
    void stop();

private:
    struct Impl;
    explicit CarrierServer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct CarrierBridgeConfig {
    std::string host;
    uint16_t port = 0;
    CarrierTransport transport = CarrierTransport::kTcp;
    std::chrono::milliseconds connect_timeout{5000};
    // host_id and token select a host on a shared carrier, which multiplexes
    // every host on one port. Leave them empty for a per-host relay port.
    std::string host_id;
    std::string token;
};

struct CarrierBridgeStats {
    uint64_t to_carrier_datagrams = 0;
    uint64_t to_carrier_bytes = 0;
    uint64_t from_carrier_datagrams = 0;
    uint64_t from_carrier_bytes = 0;
};

class CarrierBridge {
public:
    // Connects the carrier and binds the loopback UDP endpoint. The QUIC
    // client can dial local_port() immediately afterwards.
    static std::unique_ptr<CarrierBridge> open(const CarrierBridgeConfig& config,
                                               std::string* error);
    ~CarrierBridge();
    CarrierBridge(const CarrierBridge&) = delete;
    CarrierBridge& operator=(const CarrierBridge&) = delete;

    uint16_t local_port() const;
    CarrierTransport transport() const;
    CarrierBridgeStats stats() const;
    // Why the carrier stopped after open(), empty while it is healthy.
    std::string error() const;
    void stop();

private:
    struct Impl;
    explicit CarrierBridge(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// RFC 6455 Sec-WebSocket-Accept for a client key. Exposed for the codec test;
// there is no other reason to compute it outside a handshake.
std::string carrier_websocket_accept(const std::string& key);

}  // namespace chast
