#include "service_log.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "chast/host_settings.h"

namespace chast::host {

namespace {

constexpr uintmax_t kMaxLogBytes = 5u * 1024u * 1024u;

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

void rotate_if_large(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size <= kMaxLogBytes) {
        return;
    }
    const std::filesystem::path old = path.string() + ".old";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(path, old, ec);
}

std::string timestamp() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear,
                  utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
                  utc.wMilliseconds);
    return text;
}

// Length of the valid UTF-8 sequence starting at `i`, or 0 when the byte
// cannot start one. Overlong forms, surrogates and values past U+10FFFF are
// treated as invalid so a log reader's JSON parser never chokes on the file.
size_t utf8_sequence_length(const std::string_view text, size_t i) {
    const auto byte = [&text](size_t at) {
        return static_cast<unsigned char>(text[at]);
    };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        return 1;
    }
    size_t tail = 0;
    uint32_t code_point = 0;
    if ((lead & 0xE0) == 0xC0) {
        tail = 1;
        code_point = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        tail = 2;
        code_point = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        tail = 3;
        code_point = lead & 0x07u;
    } else {
        return 0;
    }
    if (i + tail >= text.size()) {
        return 0;
    }
    for (size_t j = 1; j <= tail; ++j) {
        const unsigned char next = byte(i + j);
        if ((next & 0xC0) != 0x80) {
            return 0;
        }
        code_point = (code_point << 6) | (next & 0x3Fu);
    }
    if ((tail == 1 && code_point < 0x80) || (tail == 2 && code_point < 0x800) ||
        (tail == 3 && code_point < 0x10000) ||
        (code_point >= 0xD800 && code_point <= 0xDFFF) || code_point > 0x10FFFF) {
        return 0;
    }
    return tail + 1;
}

void append_escaped(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            switch (byte) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (byte < 0x20) {
                        out += "\\u00";
                        out.push_back(kHex[byte >> 4]);
                        out.push_back(kHex[byte & 0x0Fu]);
                    } else {
                        out.push_back(static_cast<char>(byte));
                    }
                    break;
            }
            ++i;
            continue;
        }
        const size_t length = utf8_sequence_length(text, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        out.append(text.data() + i, length);
        i += length;
    }
    out.push_back('"');
}

void append_record(std::string& out, std::string_view event,
                   std::initializer_list<LogField> fields) {
    out += "{\"ts\":";
    append_escaped(out, timestamp());
    out += ",\"event\":";
    append_escaped(out, event);
    out += ",\"fields\":{";
    bool first = true;
    for (const LogField& field : fields) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        append_escaped(out, field.key);
        out.push_back(':');
        append_escaped(out, field.value);
    }
    out += "}}\n";
}

void write_line(const std::string& path, std::string& line) {
    if (path.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(log_mutex());
    std::error_code ec;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    rotate_if_large(file);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        return;
    }
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace

std::string service_log_path(const char* name) {
    const std::string directory = machine_settings_directory();
    if (directory.empty()) {
        return {};
    }
    return directory + "\\" + name;
}

void log_event(const std::string& path, std::string_view event,
               std::initializer_list<LogField> fields) {
    if (path.empty()) {
        return;
    }
    std::string line;
    append_record(line, event, fields);
    write_line(path, line);
}

void log_line(const std::string& path, const std::string& message) {
    log_event(path, "message", {{"text", message}});
}

}  // namespace chast::host
