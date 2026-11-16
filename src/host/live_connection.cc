// Native control plane for targeted viewer disconnects.
//
// Two files carry this state, with exactly one writer each:
//
//   <policy state>.live-connections           this process: snapshot + receipts
//   <policy state>.live-connections.commands  chast-tunnel: verified commands
//
// The split matters because a crash between the HTTP reply and the native read
// must not lose a command the server already counts as delivered, and a
// restarted host must not resurrect a previous runtime's queue.

#include "chast/live_connection.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

#include "chast/security.h"

namespace chast::host {
namespace {

constexpr size_t kMaxStateFile = 256 * 1024;
constexpr size_t kMaxReceipts = 32;
constexpr size_t kMaxSnapshotConnections = 256;
constexpr size_t kMaxDetail = 512;
constexpr size_t kMaxCommandId = 64;
constexpr uint64_t kMaxSequence = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());

int64_t wall_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string bounded(const std::string& text, size_t limit) {
    return text.size() <= limit ? text : text.substr(0, limit);
}

// The rest of the host validates text this way, so a peer-supplied address or
// display name cannot smuggle invalid bytes into JSON the agent must sign.
bool valid_utf8(const std::string& text) {
    return text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                               static_cast<int>(text.size()), nullptr, 0) > 0;
}

std::string quote(const std::string& text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + '"';
}

// A deliberately small JSON reader: objects, arrays, strings, integers and
// booleans. Unknown members are skipped so the agent may add fields, but every
// value this file depends on must have the expected shape.
struct Json {
    enum class Kind { Object, Array, String, Integer, Boolean };
    Kind kind = Kind::Object;
    std::map<std::string, Json> members;
    std::vector<Json> items;
    std::string text;
    int64_t integer = 0;
    bool boolean = false;

    const Json* find(const char* key) const {
        const auto found = members.find(key);
        return found == members.end() ? nullptr : &found->second;
    }
    bool is_string(const char* key, std::string* out) const {
        const Json* value = find(key);
        if (!value || value->kind != Kind::String) return false;
        if (out) *out = value->text;
        return true;
    }
    bool is_integer(const char* key, int64_t* out) const {
        const Json* value = find(key);
        if (!value || value->kind != Kind::Integer) return false;
        if (out) *out = value->integer;
        return true;
    }
    bool is_boolean(const char* key, bool* out) const {
        const Json* value = find(key);
        if (!value || value->kind != Kind::Boolean) return false;
        if (out) *out = value->boolean;
        return true;
    }
};

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    bool read(Json& out) {
        if (text_.empty() || text_.size() > kMaxStateFile) return false;
        if (!value(out, 0)) return false;
        space();
        return pos_ == text_.size();
    }

private:
    void space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\r' ||
                                      text_[pos_] == '\n' || text_[pos_] == '\t'))
            ++pos_;
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
            const int digit = c >= '0' && c <= '9' ? c - '0'
                            : c >= 'a' && c <= 'f' ? c - 'a' + 10
                            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                   : -1;
            if (digit < 0) return false;
            out = (out << 4) | static_cast<uint32_t>(digit);
        }
        return true;
    }
    static void utf8(uint32_t code, std::string& out) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xc0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 63));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 63));
            out += static_cast<char>(0x80 | (code & 63));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 63));
            out += static_cast<char>(0x80 | ((code >> 6) & 63));
            out += static_cast<char>(0x80 | (code & 63));
        }
    }
    bool string(std::string& out) {
        if (!take('"')) return false;
        out.clear();
        while (pos_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return valid_utf8(out);
            if (c < 0x20) return false;
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
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
                } else if (code >= 0xdc00 && code <= 0xdfff) {
                    return false;
                }
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
                if (!string(key) || !take(':') || !value(child, depth + 1)) return false;
                out.members[std::move(key)] = std::move(child);
                if (take('}')) return true;
                if (!take(',')) return false;
            }
        }
        if (c == '[') {
            ++pos_;
            out.kind = Json::Kind::Array;
            if (take(']')) return true;
            for (;;) {
                Json child;
                if (!value(child, depth + 1)) return false;
                out.items.push_back(std::move(child));
                if (take(']')) return true;
                if (!take(',')) return false;
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
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

bool to_wide(const std::string& text, std::wstring& out) {
    if (text.empty() || text.find('\0') != std::string::npos) return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return false;
    out.assign(static_cast<size_t>(length), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), out.data(), length) == length;
}

// A missing file is empty state, not an error: the first tick creates it.
bool read_text_file(const std::string& path, std::string& out, std::string* error) {
    std::wstring wide;
    if (!to_wide(path, wide)) {
        if (error) *error = "Invalid live connection state path.";
        return false;
    }
    std::ifstream file(std::filesystem::path(wide), std::ios::binary);
    if (!file) {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(wide), ec)) {
            out.clear();
            return true;
        }
        if (error) *error = "Cannot open the live connection state file.";
        return false;
    }
    out.assign(kMaxStateFile + 1, '\0');
    file.read(out.data(), static_cast<std::streamsize>(out.size()));
    if (file.bad()) {
        if (error) *error = "Cannot read the live connection state file.";
        return false;
    }
    out.resize(static_cast<size_t>(file.gcount()));
    if (out.size() > kMaxStateFile) {
        if (error) *error = "The live connection state file is too large.";
        return false;
    }
    return true;
}

// Flush, then rename over the target: a reader sees either the previous file
// or the complete new one, never a partial write.
bool write_text_file(const std::string& path, const std::string& contents, std::string* error) {
    std::wstring wide;
    if (!to_wide(path, wide)) {
        if (error) *error = "Invalid live connection state path.";
        return false;
    }
    const std::filesystem::path target(wide);
    std::error_code ec;
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        const std::string detail = "Cannot create the live connection state directory (" +
                                   std::to_string(static_cast<unsigned long>(ec.value())) +
                                   ").";
        if (error) *error = detail;
        return false;
    }
    const std::filesystem::path temporary =
        target.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            if (error) *error = "Cannot write the live connection state file.";
            return false;
        }
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file) {
            std::filesystem::remove(temporary, ec);
            if (error) *error = "Cannot write the live connection state file.";
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = "Cannot replace the live connection state file.";
        return false;
    }
    return true;
}

std::string session_mode_name(SessionMode mode) {
    switch (mode) {
    case SessionMode::Desktop: return "desktop";
    case SessionMode::FileTransfer: return "file_transfer";
    case SessionMode::Terminal: return "terminal";
    case SessionMode::Camera: return "camera";
    case SessionMode::PortForward: return "port_forward";
    }
    return "desktop";
}

std::string serialize_state(const LiveConnectionState& state) {
    std::string out = "{\"version\":" + std::to_string(state.version) +
                      ",\"runtime_id\":" + quote(state.runtime_id) +
                      ",\"sequence\":" + std::to_string(state.sequence) +
                      ",\"terminal\":" + (state.terminal ? "true" : "false") +
                      ",\"connections\":[";
    bool first = true;
    for (const auto& item : state.connections) {
        if (!first) out += ',';
        first = false;
        out += "{\"connection_id\":" + quote(item.connection_id) +
               ",\"peer_address\":" + quote(item.peer_address) +
               ",\"viewer\":" + quote(item.viewer) +
               ",\"mode\":" + quote(item.mode) +
               ",\"route\":" + quote(item.route) +
               ",\"permissions\":" + std::to_string(item.permissions) +
               ",\"controller\":" + (item.controller ? "true" : "false") +
               ",\"started_at_ms\":" + std::to_string(item.started_at_ms) + "}";
    }
    out += "],\"receipts\":[";
    first = true;
    for (const auto& receipt : state.receipts) {
        if (!first) out += ',';
        first = false;
        out += "{\"receipt_sequence\":" + std::to_string(receipt.sequence) +
               ",\"command_id\":" + quote(receipt.command_id) +
               ",\"status\":" + quote(receipt.status) +
               ",\"confirmed_absent\":" + (receipt.confirmed_absent ? "true" : "false") +
               ",\"detail\":" + quote(receipt.detail) + "}";
    }
    out += "]}\n";
    return out;
}

bool same_connections(const std::vector<LiveConnectionInfo>& left,
                      const std::vector<LiveConnectionInfo>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const auto& a = left[i];
        const auto& b = right[i];
        if (a.connection_id != b.connection_id || a.peer_address != b.peer_address ||
            a.viewer != b.viewer || a.mode != b.mode || a.route != b.route ||
            a.permissions != b.permissions || a.controller != b.controller ||
            a.started_at_ms != b.started_at_ms)
            return false;
    }
    return true;
}

bool parse_receipt(const Json& json, LiveConnectionReceipt& out) {
    int64_t sequence = 0;
    out = LiveConnectionReceipt{};
    if (!json.is_integer("receipt_sequence", &sequence) || sequence < 1) return false;
    out.sequence = static_cast<uint64_t>(sequence);
    if (!json.is_string("command_id", &out.command_id) || out.command_id.empty()) return false;
    if (!json.is_string("status", &out.status)) return false;
    if (out.status != "applied" && out.status != "failed") return false;
    if (!json.is_boolean("confirmed_absent", &out.confirmed_absent)) out.confirmed_absent = false;
    if (out.status == "applied" && !out.confirmed_absent) return false;
    json.is_string("detail", &out.detail);
    out.detail = bounded(out.detail, kMaxDetail);
    return true;
}

std::string fresh_id() {
    std::string value = generate_live_connection_id();
    while (!valid_live_connection_id(value)) value = generate_live_connection_id();
    return value;
}

// A runtime id belongs to one native listener runtime, not to the process: the
// tray restarts its listener in place when the settings change, and the server
// refuses to resurrect a runtime that already published its terminal snapshot.
// Every id this process ever issued therefore stays reserved.
std::string next_runtime_id() {
    static std::mutex mutex;
    static std::set<std::string> issued;
    std::lock_guard<std::mutex> lock(mutex);
    std::string id = fresh_id();
    while (issued.count(id)) id = fresh_id();
    issued.insert(id);
    return id;
}

}  // namespace

bool valid_live_connection_id(const std::string& value) {
    if (value.size() != kLiveConnectionIdLength) return false;
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string generate_live_connection_id() {
    std::array<uint8_t, kLiveConnectionIdBytes> bytes{};
    if (!secure_random(bytes.data(), bytes.size())) return {};
    return hex_encode(bytes.data(), bytes.size());
}

std::string live_connection_state_path(const HostSettings& local) {
    return local.policy_state.empty() ? std::string{} : local.policy_state + ".live-connections";
}

std::string live_connection_commands_path(const HostSettings& local) {
    const std::string state = live_connection_state_path(local);
    return state.empty() ? std::string{} : state + ".commands";
}

bool read_live_connection_state(const std::string& path, LiveConnectionState& state,
                                std::string* error) {
    if (error) error->clear();
    state = LiveConnectionState{};
    if (path.empty()) return true;
    std::string text;
    if (!read_text_file(path, text, error)) return false;
    if (text.empty()) return true;
    Json root;
    if (!Reader(text).read(root) || root.kind != Json::Kind::Object) {
        if (error) *error = "The live connection state file is not valid JSON.";
        return false;
    }
    int64_t version = 0;
    if (!root.is_integer("version", &version) || version != 1) {
        if (error) *error = "Unsupported live connection state version.";
        return false;
    }
    state.version = 1;
    if (!root.is_string("runtime_id", &state.runtime_id)) state.runtime_id.clear();
    int64_t sequence = 0;
    if (root.is_integer("sequence", &sequence) && sequence >= 0) {
        state.sequence = static_cast<uint64_t>(sequence);
    }
    root.is_boolean("terminal", &state.terminal);
    if (const Json* connections = root.find("connections")) {
        if (connections->kind != Json::Kind::Array) {
            if (error) *error = "The live connection snapshot is malformed.";
            return false;
        }
        for (const Json& item : connections->items) {
            LiveConnectionInfo info;
            if (!item.is_string("connection_id", &info.connection_id) ||
                !valid_live_connection_id(info.connection_id)) {
                if (error) *error = "The live connection snapshot has an invalid id.";
                return false;
            }
            item.is_string("peer_address", &info.peer_address);
            item.is_string("viewer", &info.viewer);
            item.is_string("mode", &info.mode);
            item.is_string("route", &info.route);
            int64_t permissions = 0;
            if (item.is_integer("permissions", &permissions) && permissions >= 0)
                info.permissions = static_cast<uint32_t>(permissions);
            item.is_boolean("controller", &info.controller);
            item.is_integer("started_at_ms", &info.started_at_ms);
            state.connections.push_back(std::move(info));
        }
    }
    if (const Json* receipts = root.find("receipts")) {
        if (receipts->kind != Json::Kind::Array) {
            if (error) *error = "The live connection receipts are malformed.";
            return false;
        }
        for (const Json& item : receipts->items) {
            LiveConnectionReceipt receipt;
            if (!parse_receipt(item, receipt)) continue;
            state.receipts.push_back(std::move(receipt));
        }
    }
    return true;
}

bool write_live_connection_state(const std::string& path, const LiveConnectionState& state,
                                std::string* error) {
    if (error) error->clear();
    if (path.empty()) {
        if (error) *error = "The live connection state path is not configured.";
        return false;
    }
    if (!valid_live_connection_id(state.runtime_id)) {
        if (error) *error = "The live connection state has an invalid runtime id.";
        return false;
    }
    return write_text_file(path, serialize_state(state), error);
}

bool read_live_connection_commands(const std::string& path, const std::string& runtime_id,
                                   std::vector<LiveConnectionCommand>& out,
                                   std::string* error) {
    if (error) error->clear();
    out.clear();
    if (path.empty()) return true;
    std::string text;
    if (!read_text_file(path, text, error)) return false;
    if (text.empty()) return true;
    Json root;
    if (!Reader(text).read(root) || root.kind != Json::Kind::Object) {
        if (error) *error = "The live connection command queue is not valid JSON.";
        return false;
    }
    int64_t version = 0;
    if (!root.is_integer("version", &version) || version != 1) {
        if (error) *error = "Unsupported live connection command queue version.";
        return false;
    }
    std::string queued_runtime;
    if (!root.is_string("runtime_id", &queued_runtime) || queued_runtime != runtime_id) {
        // Commands for another process are not this process's to apply.
        return true;
    }
    const Json* commands = root.find("commands");
    if (!commands) return true;
    if (commands->kind != Json::Kind::Array) {
        if (error) *error = "The live connection command queue is malformed.";
        return false;
    }
    for (const Json& item : commands->items) {
        LiveConnectionCommand command;
        if (item.is_string("command_id", &command.command_id) &&
            item.is_string("connection_id", &command.connection_id) &&
            item.is_string("operation", &command.operation)) {
            item.is_string("reason", &command.reason);
            item.is_integer("issued_at_ms", &command.issued_at_ms);
            item.is_integer("deadline_ms", &command.deadline_ms);
            out.push_back(std::move(command));
        }
    }
    return true;
}

LiveConnectionSource live_connection_source(HostServer& server) {
    LiveConnectionSource source;
    source.snapshot = [&server] {
        std::vector<LiveConnectionInfo> raw;
        const HostStatus status = server.status();
        for (const auto& viewer : server.viewers()) {
            LiveConnectionInfo info;
            info.native_connection_id = viewer.id;
            info.peer_address = viewer.peer_address;
            info.mode = session_mode_name(viewer.mode);
            info.route = status.carrier_active > 0 ? "carrier" : "direct";
            info.permissions = viewer.approved_permissions;
            info.controller = viewer.controls_input;
            raw.push_back(std::move(info));
        }
        return raw;
    };
    source.disconnect = [&server](uint64_t native_id) {
        return server.disconnect_viewer(native_id);
    };
    return source;
}

LiveConnectionController::LiveConnectionController(std::string state_path)
    : state_path_(std::move(state_path)) {
    if (state_path_.empty()) return;
    commands_path_ = state_path_ + ".commands";
    start_runtime();
}

void LiveConnectionController::start_runtime() {
    if (!enabled()) return;
    runtime_id_ = next_runtime_id();
    // Nothing from the previous runtime carries over: its sequence, its
    // receipts and its command bookkeeping all ended with it. `issued_` is
    // deliberately kept so a stale wire id is still answered as already
    // ended instead of resolving to a viewer of this runtime.
    state_ = LiveConnectionState{};
    wire_by_native_.clear();
    started_at_ms_.clear();
    pending_.clear();
    next_receipt_sequence_ = 0;
    ended_ = false;
    last_error_.clear();
}

std::vector<LiveConnectionInfo> LiveConnectionController::map_connections(
    const std::vector<LiveConnectionInfo>& raw) {
    const int64_t now = wall_clock_ms();
    std::map<uint64_t, bool> seen;
    std::vector<LiveConnectionInfo> out;
    out.reserve(raw.size());
    for (const auto& item : raw) {
        if (item.native_connection_id == 0) continue;
        seen[item.native_connection_id] = true;
        auto found = wire_by_native_.find(item.native_connection_id);
        if (found == wire_by_native_.end()) {
            std::string id = fresh_id();
            while (issued_.count(id)) id = fresh_id();
            issued_.insert(id);
            found = wire_by_native_.emplace(item.native_connection_id, id).first;
            started_at_ms_[id] = now;
        }
        LiveConnectionInfo info = item;
        info.connection_id = found->second;
        info.peer_address = bounded(info.peer_address, 128);
        info.viewer = bounded(info.viewer, 128);
        info.mode = bounded(info.mode, 32);
        info.route = bounded(info.route, 32);
        // Text the host cannot validate would poison the signed JSON, so it
        // is dropped rather than reported.
        if (!valid_utf8(info.peer_address)) info.peer_address.clear();
        if (!valid_utf8(info.viewer)) info.viewer.clear();
        if (!valid_utf8(info.mode)) info.mode = "desktop";
        if (!valid_utf8(info.route)) info.route = "direct";
        const auto started = started_at_ms_.find(info.connection_id);
        info.started_at_ms = started == started_at_ms_.end() ? now : started->second;
        out.push_back(std::move(info));
    }
    // A departed native id is forgotten, but its wire id stays in `issued_`:
    // a later command naming it must be answered as "already ended", never
    // retargeted at a newer viewer.
    for (auto it = wire_by_native_.begin(); it != wire_by_native_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        started_at_ms_.erase(it->second);
        it = wire_by_native_.erase(it);
    }
    std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
        return left.connection_id < right.connection_id;
    });
    if (out.size() > kMaxSnapshotConnections) out.resize(kMaxSnapshotConnections);
    return out;
}

// A receipt is written once per command. Repeated ticks therefore never
// advance the receipt sequence for the same command.
bool LiveConnectionController::settle(const std::string& command_id, const std::string& status,
                                      bool confirmed_absent, const std::string& detail) {
    if (command_id.empty()) return false;
    for (const auto& receipt : state_.receipts) {
        if (receipt.command_id == command_id) return false;
    }
    if (next_receipt_sequence_ >= kMaxSequence) return false;
    LiveConnectionReceipt receipt;
    receipt.sequence = ++next_receipt_sequence_;
    receipt.command_id = command_id;
    receipt.status = status;
    receipt.confirmed_absent = confirmed_absent;
    receipt.detail = bounded(detail, kMaxDetail);
    state_.receipts.push_back(std::move(receipt));
    while (state_.receipts.size() > kMaxReceipts) state_.receipts.erase(state_.receipts.begin());
    return true;
}

bool LiveConnectionController::run(const LiveConnectionSource& source, bool terminal,
                                   std::string* error) {
    if (error) error->clear();
    last_error_.clear();
    if (!enabled()) return true;
    if (!source.snapshot || (!terminal && !source.disconnect)) {
        last_error_ = "the live connection source is incomplete";
        if (error) *error = last_error_;
        return false;
    }
    if (ended_ && !terminal) {
        // The terminal snapshot is already on disk, so this runtime is over:
        // the server rejects any later live snapshot under the same id as a
        // resurrection. Only start_runtime() may publish again.
        last_error_ = "the host runtime already published its terminal snapshot";
        if (error) *error = last_error_;
        return false;
    }
    const std::string& runtime_id = runtime_id_;
    if (!valid_live_connection_id(runtime_id)) {
        last_error_ = "the host runtime id could not be generated";
        if (error) *error = last_error_;
        return false;
    }

    std::vector<LiveConnectionInfo> current;
    if (!terminal) current = map_connections(source.snapshot());
    bool changed = state_.runtime_id != runtime_id || state_.sequence == 0 ||
                   state_.terminal != terminal || !same_connections(state_.connections, current);
    bool requested_disconnect = false;
    // Commands issued during this call are settled no earlier than the next
    // tick, so a receipt always follows an observation of the viewer set made
    // after the request was handed to the host.
    std::set<std::string> requested_now;
    const int64_t now = wall_clock_ms();

    if (!terminal) {
        std::vector<LiveConnectionCommand> commands;
        std::string command_error;
        if (!read_live_connection_commands(commands_path_, runtime_id, commands, &command_error)) {
            // A corrupt inbox is never acted on: report it and keep both the
            // last known snapshot and the settled receipts.
            last_error_ = command_error;
            if (error) *error = command_error;
            return false;
        }
        std::map<std::string, uint64_t> native_by_wire;
        for (const auto& [native, wire] : wire_by_native_) native_by_wire[wire] = native;
        for (const auto& command : commands) {
            if (command.command_id.empty() || command.command_id.size() > kMaxCommandId) continue;
            if (pending_.count(command.command_id)) continue;
            bool settled = false;
            for (const auto& receipt : state_.receipts) {
                if (receipt.command_id == command.command_id) settled = true;
            }
            if (settled) continue;
            if (command.operation != "disconnect") {
                changed |= settle(command.command_id, "failed", false, "unsupported operation");
                continue;
            }
            if (!valid_live_connection_id(command.connection_id)) {
                changed |= settle(command.command_id, "failed", false, "invalid connection id");
                continue;
            }
            const auto live = native_by_wire.find(command.connection_id);
            if (live == native_by_wire.end()) {
                if (issued_.count(command.connection_id)) {
                    // The viewer ended before the command arrived. The wanted
                    // end state already holds and is verified by the absence.
                    changed |= settle(command.command_id, "applied", true,
                                      "connection had already ended");
                } else {
                    changed |= settle(command.command_id, "failed", false,
                                      "connection is unknown to this runtime");
                }
                continue;
            }
            if (command.deadline_ms <= now) {
                changed |= settle(command.command_id, "failed", false,
                                  "command expired before the native host applied it");
                continue;
            }
            if (!source.disconnect(live->second)) {
                changed |= settle(command.command_id, "failed", false,
                                  "the host refused the disconnect request");
                continue;
            }
            PendingCommand pending;
            pending.connection_id = command.connection_id;
            pending.deadline_ms = command.deadline_ms;
            requested_now.insert(command.command_id);
            pending_.emplace(command.command_id, std::move(pending));
            requested_disconnect = true;
        }
        if (requested_disconnect) {
            // The snapshot above predates the disconnect requests. Re-read the
            // viewer set so the file the operator sees already reflects them,
            // and so "absent" below is judged on the state after the request.
            current = map_connections(source.snapshot());
            changed |= !same_connections(state_.connections, current);
        }
    }

    std::map<std::string, bool> present_now;
    for (const auto& item : current) present_now[item.connection_id] = true;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (requested_now.count(it->first)) {
            ++it;
            continue;
        }
        if (!present_now.count(it->second.connection_id)) {
            // "applied" is only written once the viewer set verifiably no
            // longer holds the target, which is what confirmed_absent means.
            changed |= settle(it->first, "applied", true, "");
            it = pending_.erase(it);
            continue;
        }
        if (now > it->second.deadline_ms) {
            changed |= settle(it->first, "failed", false,
                              "the connection is still present after the disconnect deadline");
            it = pending_.erase(it);
            continue;
        }
        ++it;
    }

    if (!changed) return true;

    state_.version = 1;
    state_.runtime_id = runtime_id;
    state_.terminal = terminal;
    state_.connections = std::move(current);
    if (state_.sequence < kMaxSequence) ++state_.sequence;
    std::string write_error;
    if (!write_live_connection_state(state_path_, state_, &write_error)) {
        last_error_ = write_error;
        if (error) *error = write_error;
        return false;
    }
    if (terminal) ended_ = true;
    return true;
}

bool LiveConnectionController::tick(HostServer& server, std::string* error) {
    return run(live_connection_source(server), false, error);
}

bool LiveConnectionController::tick(const LiveConnectionSource& source, std::string* error) {
    return run(source, false, error);
}

bool LiveConnectionController::publish_terminal(HostServer& server, std::string* error) {
    return run(live_connection_source(server), true, error);
}

bool LiveConnectionController::publish_terminal(const LiveConnectionSource& source,
                                                std::string* error) {
    return run(source, true, error);
}

}  // namespace chast::host
