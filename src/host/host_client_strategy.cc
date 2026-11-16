#include "chast/host_client_strategy.h"

#include <windows.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string_view>
#include <utility>

#include "chast/access_ticket.h"

namespace chast {
namespace {
constexpr size_t kMaxCache = 64 * 1024;
constexpr int64_t kMaxRevision = (int64_t{1} << 53) - 1;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

// This wire schema contains objects and scalar values only. Reject unsupported
// JSON shapes instead of silently extracting a substring or coercing types.
struct Json {
    enum class Kind { Object, String, Integer, Boolean } kind = Kind::Object;
    std::map<std::string, Json> members;
    std::string text;
    int64_t integer = 0;
    bool boolean = false;
};

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}
    bool read(Json& out) {
        return text_.size() <= kMaxCache && value(out, 0) && (space(), pos_ == text_.size());
    }
private:
    void space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\r' ||
               text_[pos_] == '\n' || text_[pos_] == '\t')) ++pos_;
    }
    bool take(char c) {
        space();
        if (pos_ == text_.size() || text_[pos_] != c) return false;
        ++pos_;
        return true;
    }
    bool hex4(uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == text_.size()) return false;
            const char c = text_[pos_++];
            const int n = c >= '0' && c <= '9' ? c - '0' :
                          c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (n < 0) return false;
            out = (out << 4) | static_cast<uint32_t>(n);
        }
        return true;
    }
    static void utf8(uint32_t c, std::string& out) {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 63));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    bool string(std::string& out) {
        if (!take('"')) return false;
        out.clear();
        while (pos_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') {
                return out.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    out.data(), static_cast<int>(out.size()), nullptr, 0) > 0;
            }
            if (c < 0x20) return false;
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (pos_ == text_.size()) return false;
            switch (text_[pos_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t code = 0;
                if (!hex4(code)) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (pos_ + 2 > text_.size() || text_.substr(pos_, 2) != "\\u") return false;
                    pos_ += 2;
                    uint32_t low = 0;
                    if (!hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                utf8(code, out);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json& out, unsigned depth) {
        space();
        if (depth > 8 || pos_ == text_.size()) return false;
        const char c = text_[pos_];
        if (c == '{') {
            ++pos_;
            out.kind = Json::Kind::Object;
            if (take('}')) return true;
            for (;;) {
                std::string key;
                Json child;
                if (!string(key) || !take(':') || !value(child, depth + 1) ||
                    !out.members.emplace(std::move(key), std::move(child)).second) return false;
                if (take('}')) return true;
                if (!take(',')) return false;
            }
        }
        if (c == '"') { out.kind = Json::Kind::String; return string(out.text); }
        for (const auto literal : {std::string_view("true"), std::string_view("false")}) {
            if (text_.substr(pos_, literal.size()) == literal) {
                pos_ += literal.size();
                out.kind = Json::Kind::Boolean;
                out.boolean = literal == "true";
                return true;
            }
        }
        const size_t begin = pos_;
        if (c == '-') ++pos_;
        const size_t digits = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (pos_ == digits || (pos_ > digits + 1 && text_[digits] == '0')) return false;
        out.kind = Json::Kind::Integer;
        const auto parsed = std::from_chars(text_.data() + begin, text_.data() + pos_, out.integer);
        return parsed.ec == std::errc{} && parsed.ptr == text_.data() + pos_;
    }
    std::string_view text_;
    size_t pos_ = 0;
};

std::string quote(const std::string& text) {
    // Match encoding/json, including HTML and JavaScript line separators.
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == '<' || c == '>' || c == '&') {
                out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
            } else if (c == 0xe2 && i + 2 < text.size() &&
                       static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                       (static_cast<unsigned char>(text[i + 2]) == 0xa8 ||
                        static_cast<unsigned char>(text[i + 2]) == 0xa9)) {
                out += static_cast<unsigned char>(text[i + 2]) == 0xa8 ? "\\u2028" : "\\u2029";
                i += 2;
            } else out += static_cast<char>(c);
        }
    }
    return out + '"';
}

bool sha_hex(const std::string& text, std::string& out) {
    std::array<uint8_t, 32> digest;
    if (!sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest)) return false;
    out = hex_encode(digest.data(), digest.size());
    return true;
}

bool fields(const Json& object, std::initializer_list<const char*> required,
            std::initializer_list<const char*> optional = {}) {
    if (object.kind != Json::Kind::Object) return false;
    for (const auto key : required) if (!object.members.contains(key)) return false;
    for (const auto& [key, value] : object.members) {
        bool known = false;
        for (const auto allowed : required) known |= key == allowed;
        for (const auto allowed : optional) known |= key == allowed;
        if (!known) return false;
    }
    return true;
}

bool text_is(const Json& object, const char* key, const std::string& wanted) {
    const auto found = object.members.find(key);
    return found != object.members.end() && found->second.kind == Json::Kind::String &&
           found->second.text == wanted;
}

constexpr const char* setting_keys[] = {
    "host.lock_after_session_end", "host.keep_awake", "host.auto_disconnect_minutes",
    "host.max_viewers", "host.carrier", "host.lan_discovery"
};
static_assert(std::size(setting_keys) == static_cast<size_t>(HostPreference::Count));

bool setting(const std::string& key, const Json& json, HostStrategySetting& out) {
    if (!fields(json, {"mode", "value"}) ||
        (!text_is(json, "mode", "locked") && !text_is(json, "mode", "default"))) return false;
    if (key.empty() || key.size() > 128) return false;
    for (char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    }
    out.locked = text_is(json, "mode", "locked");
    for (size_t i = 0; i < std::size(setting_keys); ++i) {
        if (key == setting_keys[i]) out.preference = static_cast<HostPreference>(i);
    }
    const auto& value = json.members.at("value");
    if (value.kind == Json::Kind::Boolean) out.canonical_value = value.boolean ? "true" : "false";
    else if (value.kind == Json::Kind::Integer) out.canonical_value = std::to_string(value.integer);
    else if (value.kind == Json::Kind::String) out.canonical_value = quote(value.text);
    else return false;
    if (out.preference == HostPreference::Count) return true;
    if (out.preference == HostPreference::AutoDisconnectMinutes || out.preference == HostPreference::MaxViewers) {
        const int64_t min = out.preference == HostPreference::MaxViewers ? 1 : 0;
        const int64_t max = out.preference == HostPreference::MaxViewers ? 8 : 10080;
        if (value.kind != Json::Kind::Integer || value.integer < min || value.integer > max) return false;
        out.value = static_cast<uint32_t>(value.integer);
    } else {
        if (value.kind != Json::Kind::Boolean) return false;
        out.value = value.boolean;
    }
    return true;
}
} // namespace

bool host_strategy_identity(const HostSettings& local, const std::string& server_pin,
                            HostStrategyIdentity& output, std::string* error) {
    if (error) error->clear();
    if (local.server_url.empty() || local.tunnel_id.empty() || local.tunnel_key.empty())
        return fail(error, "Client strategy identity is not configured.");
    HostStrategyIdentity identity;
    identity.server_url = local.server_url;
    while (!identity.server_url.empty() && identity.server_url.back() == '/') identity.server_url.pop_back();
    identity.server_public_key = server_pin;
    identity.peer_id = local.tunnel_id;
    std::string secret = "chast-host-identity-v1\n" + local.tunnel_key;
    std::array<uint8_t, 32> seed{}, pub{};
    const bool hashed = sha256(reinterpret_cast<const uint8_t*>(secret.data()), secret.size(), seed);
    SecureZeroMemory(secret.data(), secret.size());
    if (!hashed)
        return fail(error, "Cannot derive client strategy identity.");
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    SecureZeroMemory(seed.data(), seed.size());
    size_t length = pub.size();
    const bool derived = key && EVP_PKEY_get_raw_public_key(key, pub.data(), &length) == 1 && length == pub.size();
    EVP_PKEY_free(key);
    if (!derived) return fail(error, "Cannot derive client strategy public key.");
    identity.host_public_key = encode_ticket_base64url(std::vector<uint8_t>(pub.begin(), pub.end()));
    if (!sha_hex("[" + quote(identity.server_url) + "," + quote(server_pin) + "," +
                 quote(identity.peer_id) + "," + quote(identity.host_public_key) + "]", identity.scope))
        return fail(error, "Cannot calculate client strategy scope.");
    output = std::move(identity);
    return true;
}

HostStrategyTarget target_from(const Json& snapshot) {
    return text_is(snapshot, "target", "viewer") ? HostStrategyTarget::Viewer : HostStrategyTarget::Host;
}

bool parse_snapshot(const Json& snapshot, HostStrategyTarget target,
                    const HostStrategyIdentity& identity, const HostClientStrategy* previous,
                    HostClientStrategy& output, std::string* error) {
    const char* target_name = target == HostStrategyTarget::Viewer ? "viewer" : "host";
    if (!fields(snapshot, {"peer_id", "target", "revision", "settings", "digest", "updated_at_ms"}, {"policy_id"}) ||
        !text_is(snapshot, "peer_id", identity.peer_id) || !text_is(snapshot, "target", target_name))
        return fail(error, "Invalid client strategy snapshot identity.");
    const auto& revision = snapshot.members.at("revision");
    const auto& digest = snapshot.members.at("digest");
    const auto& updated = snapshot.members.at("updated_at_ms");
    const auto& settings = snapshot.members.at("settings");
    if (revision.kind != Json::Kind::Integer || revision.integer < 1 || revision.integer > kMaxRevision ||
        digest.kind != Json::Kind::String || digest.text.size() != 64 ||
        updated.kind != Json::Kind::Integer || updated.integer < 0 || settings.kind != Json::Kind::Object)
        return fail(error, "Invalid client strategy snapshot fields.");
    HostClientStrategy candidate;
    candidate.scope = identity.scope;
    candidate.revision = revision.integer;
    candidate.digest = digest.text;
    if (const auto policy = snapshot.members.find("policy_id"); policy != snapshot.members.end()) {
        if (policy->second.kind != Json::Kind::String || policy->second.text.size() > 256)
            return fail(error, "Invalid client strategy policy identifier.");
        candidate.policy_id = policy->second.text;
    }
    std::string canonical = "{";
    for (const auto& [name, value] : settings.members) {
        HostStrategySetting parsed;
        if (!setting(name, value, parsed)) return fail(error, "Invalid client strategy setting.");
        if (!name.starts_with(std::string(target_name) + "."))
            return fail(error, "Client strategy contains another target's setting.");
        if (canonical.size() > 1) canonical += ',';
        canonical += quote(name) + ":{\"mode\":" + quote(parsed.locked ? "locked" : "default") +
                     ",\"value\":" + parsed.canonical_value + "}";
        candidate.settings.emplace(name, std::move(parsed));
    }
    canonical += '}';
    std::string computed;
    if (canonical.size() > 16 * 1024 ||
        !sha_hex(std::string(target_name) + std::string(1, '\0') + canonical, computed) ||
        computed != candidate.digest)
        return fail(error, "Client strategy digest does not match settings.");
    if (previous && previous->scope == candidate.scope &&
        (candidate.revision < previous->revision ||
         (candidate.revision == previous->revision &&
          (candidate.digest != previous->digest || candidate.policy_id != previous->policy_id))))
        return fail(error, "Client strategy revision was rolled back or changed in place.");
    output = std::move(candidate);
    return true;
}

bool parse_host_client_strategy(const std::string& text, const HostStrategyIdentity& identity,
                                const HostClientStrategy* previous, HostClientStrategy& output,
                                std::string* error) {
    if (error) error->clear();
    Json root;
    if (!Reader(text).read(root) || !fields(root, {"version", "scope", "server_url", "server_public_key", "host_public_key", "snapshot"}))
        return fail(error, "Invalid client strategy cache JSON.");
    const auto& version = root.members.at("version");
    if (version.kind != Json::Kind::Integer || version.integer != 1)
        return fail(error, "Unsupported client strategy cache version.");
    if (identity.scope.empty() || !text_is(root, "scope", identity.scope) ||
        !text_is(root, "server_url", identity.server_url) ||
        !text_is(root, "server_public_key", identity.server_public_key) ||
        !text_is(root, "host_public_key", identity.host_public_key))
        return fail(error, "Client strategy belongs to a different authority or device key.");
    const auto& snapshot = root.members.at("snapshot");
    return parse_snapshot(snapshot, target_from(snapshot), identity, previous, output, error);
}

bool parse_host_strategy_snapshot(const std::string& text, HostStrategyTarget target,
                                  const HostStrategyIdentity& identity,
                                  const HostClientStrategy* previous, HostClientStrategy& output,
                                  std::string* error) {
    if (error) error->clear();
    Json snapshot;
    if (!Reader(text).read(snapshot)) return fail(error, "Invalid client strategy snapshot JSON.");
    return parse_snapshot(snapshot, target, identity, previous, output, error);
}

bool read_host_client_strategy(const std::string& path, const HostStrategyIdentity& identity,
                               const HostClientStrategy* previous, HostClientStrategy& output,
                               std::string* error) {
    if (path.empty() || path.find('\0') != std::string::npos)
        return fail(error, "Invalid client strategy cache path.");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                          static_cast<int>(path.size()), nullptr, 0);
    if (length <= 0) return fail(error, "Invalid client strategy cache path.");
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), wide.data(), length);
    std::ifstream file(std::filesystem::path(wide), std::ios::binary);
    if (!file) return fail(error, "Cannot read client strategy cache.");
    std::string text(kMaxCache + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad()) return fail(error, "Client strategy cache read failed.");
    text.resize(static_cast<size_t>(file.gcount()));
    return parse_host_client_strategy(text, identity, previous, output, error);
}

HostSettings host_strategy_effective_settings(const HostSettings& local,
                                              const HostClientStrategy& strategy) {
    HostSettings effective = local;
    effective.managed_runtime_settings = true;
    effective.managed_locked_preferences = 0;
    for (const auto& [name, value] : strategy.settings) {
        if (value.locked && value.preference < HostPreference::Count)
            effective.managed_locked_preferences |= uint32_t{1} << static_cast<uint32_t>(value.preference);
        if (!value.locked && host_preference_is_explicit(local, value.preference)) continue;
        switch (value.preference) {
        case HostPreference::LockAfterSessionEnd: effective.lock_after_session_end = value.value != 0; break;
        case HostPreference::KeepAwake: effective.keep_awake = value.value != 0; break;
        case HostPreference::AutoDisconnectMinutes: effective.auto_disconnect_minutes = value.value; break;
        case HostPreference::MaxViewers: effective.max_viewers = value.value; break;
        case HostPreference::Carrier: effective.carrier = value.value != 0; break;
        case HostPreference::LanDiscovery: effective.lan_discovery = value.value != 0; break;
        default: break;
        }
    }
    return effective;
}

bool host_strategy_has_unsupported_settings(const HostClientStrategy& strategy) {
    return std::any_of(strategy.settings.begin(), strategy.settings.end(),
                       [](const auto& entry) {
                           return entry.second.preference >= HostPreference::Count;
                       });
}

bool host_strategy_start_allowed(const HostSettings& local, const std::string& cache_path,
                                 const std::string& server_pin, std::string* error) {
    if (cache_path.empty() || cache_path.find('\0') != std::string::npos)
        return fail(error, "Invalid managed client strategy cache path.");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, cache_path.data(),
                                          static_cast<int>(cache_path.size()), nullptr, 0);
    if (length <= 0) return fail(error, "Invalid managed client strategy cache path.");
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, cache_path.data(),
                        static_cast<int>(cache_path.size()), wide.data(), length);
    std::error_code ec;
    const auto status = std::filesystem::status(std::filesystem::path(wide), ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) return true;
        if (error) *error = "Cannot inspect managed client strategy cache: " + ec.message();
        return false;
    }
    if (status.type() == std::filesystem::file_type::not_found) return true;
    HostStrategyIdentity identity;
    if (!host_strategy_identity(local, server_pin, identity, error)) return false;
    HostClientStrategy strategy;
    if (!read_host_client_strategy(cache_path, identity, nullptr, strategy, error)) {
        if (error && !error->empty()) *error = "Managed client strategy is unreadable; refusing viewer admission: " + *error;
        return false;
    }
    if (host_strategy_has_unsupported_settings(strategy)) {
        return fail(error, "Managed client strategy contains settings unsupported by this host.");
    }
    return true;
}

std::string host_client_strategy_path(const HostSettings& local) {
    return local.policy_state.empty() ? std::string{} : local.policy_state + ".client-strategy";
}

std::string host_client_strategy_ack_path(const HostSettings& local) {
    const std::string cache = host_client_strategy_path(local);
    return cache.empty() ? std::string{} : cache + ".ack";
}

namespace {

const char* receipt_status_name(bool server_ready, bool tunnel_required, bool tunnel_ready,
                                bool server_failed, bool tunnel_failed,
                                HostPreference preference) {
    switch (preference) {
    case HostPreference::LockAfterSessionEnd:
    case HostPreference::AutoDisconnectMinutes:
        return server_failed ? "failed" : "pending_session";
    case HostPreference::LanDiscovery:
        if (!tunnel_required) return "applied";
        return tunnel_failed ? "failed" : tunnel_ready ? "applied" : "pending_restart";
    case HostPreference::KeepAwake:
    case HostPreference::MaxViewers:
    case HostPreference::Carrier:
        return server_failed ? "failed" : server_ready ? "applied" : "pending_restart";
    default:
        return "unsupported";
    }
}

int receipt_rank(const std::string& status) {
    if (status == "applied") return 1;
    if (status == "pending_session") return 2;
    if (status == "pending_restart") return 3;
    if (status == "unsupported") return 4;
    if (status == "failed") return 5;
    return 0;
}

bool read_small_utf8_file(const std::string& path, std::string& output) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                          static_cast<int>(path.size()), nullptr, 0);
    if (length <= 0) return false;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), wide.data(), length) != length)
        return false;
    std::ifstream file(std::filesystem::path(wide), std::ios::binary);
    if (!file) return false;
    output.assign(kMaxCache + 1, '\0');
    file.read(output.data(), static_cast<std::streamsize>(output.size()));
    if (file.bad()) return false;
    output.resize(static_cast<size_t>(file.gcount()));
    return output.size() <= kMaxCache;
}

bool write_atomic_utf8(const std::string& path, const std::string& contents,
                       std::string* error) {
    if (path.empty() || path.find('\0') != std::string::npos)
        return fail(error, "Invalid client strategy acknowledgement path.");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                          static_cast<int>(path.size()), nullptr, 0);
    if (length <= 0) return fail(error, "Invalid client strategy acknowledgement path.");
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                        static_cast<int>(path.size()), wide.data(), length);
    const std::filesystem::path target(wide);
    std::error_code ec;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return fail(error, "Cannot create client strategy acknowledgement directory.");
    const std::filesystem::path temporary = target.wstring() + L".tmp-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return fail(error, "Cannot write client strategy acknowledgement.");
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file) {
            std::filesystem::remove(temporary, ec);
            return fail(error, "Cannot write client strategy acknowledgement.");
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return fail(error, "Cannot replace client strategy acknowledgement.");
    }
    return true;
}

bool previous_receipt_sequence(const std::string& path, const std::string& scope,
                               const HostClientStrategy& strategy, int64_t& sequence,
                               std::string& previous_body) {
    sequence = 0;
    previous_body.clear();
    std::string text;
    if (!read_small_utf8_file(path, text)) return false;
    Json ack;
    if (!Reader(text).read(ack) || !fields(ack, {"scope", "receipt"}) ||
        !text_is(ack, "scope", scope)) return false;
    const Json& receipt = ack.members.at("receipt");
    if (!fields(receipt, {"revision", "digest", "sequence", "status", "results"}) ||
        receipt.members.at("revision").kind != Json::Kind::Integer ||
        receipt.members.at("revision").integer != strategy.revision ||
        !text_is(receipt, "digest", strategy.digest) ||
        receipt.members.at("sequence").kind != Json::Kind::Integer ||
        receipt.members.at("sequence").integer < 1 ||
        receipt.members.at("sequence").integer > kMaxRevision)
        return false;
    sequence = receipt.members.at("sequence").integer;
    previous_body = text;
    return true;
}

} // namespace

bool write_host_client_strategy_ack(const HostSettings& local,
                                    const HostStrategyIdentity& identity,
                                    const HostClientStrategy& strategy,
                                    bool server_ready, bool tunnel_ready,
                                    std::string* error, bool server_failed,
                                    bool tunnel_failed) {
    if (error) error->clear();
    if (identity.scope.empty() || strategy.scope != identity.scope)
        return fail(error, "Client strategy acknowledgement identity does not match.");
    const std::string path = host_client_strategy_ack_path(local);
    if (path.empty()) return fail(error, "Client strategy acknowledgement path is not configured.");
    const HostSettings effective = host_strategy_effective_settings(local, strategy);

    std::map<std::string, std::string> results;
    int overall_rank = 0;
    for (const auto& [key, setting] : strategy.settings) {
        const std::string status = receipt_status_name(
            server_ready, effective.tunnel, tunnel_ready, server_failed, tunnel_failed,
            setting.preference);
        results.emplace(key, status);
        overall_rank = (std::max)(overall_rank, receipt_rank(status));
    }
    std::string overall;
    if (results.empty()) {
        const bool tunnel_ready_or_disabled = !effective.tunnel || tunnel_ready;
        const bool tunnel_failure = effective.tunnel && tunnel_failed;
        overall = server_failed || tunnel_failure
                      ? "failed"
                      : server_ready && tunnel_ready_or_disabled ? "applied"
                                                                 : "pending_restart";
    } else {
        for (const char* status : {"applied", "pending_session", "pending_restart",
                                   "unsupported", "failed"}) {
            if (receipt_rank(status) == overall_rank) {
                overall = status;
                break;
            }
        }
    }

    std::string receipt_json = "{\"digest\":" + quote(strategy.digest) +
        ",\"results\":{";
    bool first = true;
    for (const auto& [key, status] : results) {
        if (!first) receipt_json += ',';
        first = false;
        receipt_json += quote(key) + ":{\"status\":" + quote(status) + "}";
    }
    receipt_json += "},\"revision\":" + std::to_string(strategy.revision) +
        ",\"status\":" + quote(overall);

    int64_t sequence = 0;
    std::string previous;
    const bool has_previous =
        previous_receipt_sequence(path, identity.scope, strategy, sequence, previous);
    if (sequence >= kMaxRevision)
        return fail(error, "Client strategy acknowledgement sequence is exhausted.");
    receipt_json += ",\"sequence\":" + std::to_string(sequence + 1) + "}";
    const std::string document = "{\"receipt\":" + receipt_json +
        ",\"scope\":" + quote(identity.scope) + "}\n";
    if (has_previous) {
        Json current;
        std::string body;
        if (Reader(previous).read(current) && fields(current, {"scope", "receipt"})) {
            const Json& receipt = current.members.at("receipt");
            const auto status = receipt.members.find("status");
            const auto current_results = receipt.members.find("results");
            // Comparing canonical outcome without sequence keeps retries
            // idempotent while still advancing a real state transition.
            if (status != receipt.members.end() && current_results != receipt.members.end()) {
                std::string normalized = "{\"status\":" +
                    quote(status->second.kind == Json::Kind::String ? status->second.text : "") +
                    ",\"results\":{";
                const Json& values = current_results->second;
                bool first_result = true;
                for (const auto& [key, value] : values.members) {
                    if (!first_result) normalized += ',';
                    first_result = false;
                    const auto saved = value.members.find("status");
                    normalized += quote(key) + ":{\"status\":" +
                        quote(saved != value.members.end() && saved->second.kind == Json::Kind::String
                                  ? saved->second.text : "") + "}";
                }
                normalized += "}}";
                std::string expected = "{\"status\":" + quote(overall) + ",\"results\":{";
                first = true;
                for (const auto& [key, state] : results) {
                    if (!first) expected += ',';
                    first = false;
                    expected += quote(key) + ":{\"status\":" + quote(state) + "}";
                }
                expected += "}}";
                if (normalized == expected) return true;
            }
        }
    }
    return write_atomic_utf8(path, document, error);
}
} // namespace chast
