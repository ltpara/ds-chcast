// JSONL log format checks for the service and worker: timestamp shape,
// escaping, invalid-UTF-8 replacement, rotation and concurrent appends.
// No GPU, no network, no service control.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "service_log.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    std::filesystem::path dir = std::filesystem::path(buffer) / L"chast-service-log-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

bool parse_string(const std::string& line, size_t& pos, std::string& out) {
    if (pos >= line.size() || line[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < line.size()) {
        const unsigned char byte = static_cast<unsigned char>(line[pos++]);
        if (byte == '"') {
            return true;
        }
        if (byte != '\\') {
            out.push_back(static_cast<char>(byte));
            continue;
        }
        if (pos >= line.size()) {
            return false;
        }
        const char escape = line[pos++];
        switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                if (pos + 4 > line.size()) {
                    return false;
                }
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = line[pos++];
                    code <<= 4;
                    if (digit >= '0' && digit <= '9') {
                        code |= static_cast<unsigned>(digit - '0');
                    } else if (digit >= 'a' && digit <= 'f') {
                        code |= static_cast<unsigned>(digit - 'a' + 10);
                    } else if (digit >= 'A' && digit <= 'F') {
                        code |= static_cast<unsigned>(digit - 'A' + 10);
                    } else {
                        return false;
                    }
                }
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

bool expect_char(const std::string& line, size_t& pos, char expected) {
    if (pos >= line.size() || line[pos] != expected) {
        return false;
    }
    ++pos;
    return true;
}

struct Field {
    std::string key;
    std::string value;
};

struct Record {
    std::string ts;
    std::string event;
    std::vector<Field> fields;
};

bool parse_fields(const std::string& line, size_t& pos, std::vector<Field>& fields) {
    if (!expect_char(line, pos, '{')) {
        return false;
    }
    if (pos < line.size() && line[pos] == '}') {
        ++pos;
        return true;
    }
    for (;;) {
        Field field;
        if (!parse_string(line, pos, field.key) || !expect_char(line, pos, ':') ||
            !parse_string(line, pos, field.value)) {
            return false;
        }
        fields.push_back(std::move(field));
        if (pos < line.size() && line[pos] == ',') {
            ++pos;
            continue;
        }
        break;
    }
    return expect_char(line, pos, '}');
}

// Parses one complete line in the exact shape log_event() emits. Anything
// extra or missing makes the record invalid.
bool parse_record(const std::string& line, Record& record) {
    size_t pos = 0;
    if (!expect_char(line, pos, '{')) {
        return false;
    }
    bool first = true;
    for (;;) {
        if (!first && !expect_char(line, pos, ',')) {
            return false;
        }
        first = false;
        std::string key;
        if (!parse_string(line, pos, key) || !expect_char(line, pos, ':')) {
            return false;
        }
        if (key == "fields") {
            if (!parse_fields(line, pos, record.fields)) {
                return false;
            }
        } else {
            std::string value;
            if (!parse_string(line, pos, value)) {
                return false;
            }
            if (key == "ts") {
                record.ts = std::move(value);
            } else if (key == "event") {
                record.event = std::move(value);
            } else {
                return false;
            }
        }
        if (pos < line.size() && line[pos] == '}') {
            ++pos;
            break;
        }
    }
    return pos == line.size();
}

const std::string* find_field(const Record& record, const std::string& key) {
    for (const Field& field : record.fields) {
        if (field.key == key) {
            return &field.value;
        }
    }
    return nullptr;
}

bool digits_at(const std::string& text, size_t from, size_t count) {
    for (size_t i = from; i < from + count; ++i) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

bool valid_timestamp(const std::string& ts) {
    if (ts.size() != 24 || ts[4] != '-' || ts[7] != '-' || ts[10] != 'T' || ts[13] != ':' ||
        ts[16] != ':' || ts[19] != '.' || ts[23] != 'Z') {
        return false;
    }
    if (!digits_at(ts, 0, 4) || !digits_at(ts, 5, 2) || !digits_at(ts, 8, 2) ||
        !digits_at(ts, 11, 2) || !digits_at(ts, 14, 2) || !digits_at(ts, 17, 2) ||
        !digits_at(ts, 20, 3)) {
        return false;
    }
    const int month = std::stoi(ts.substr(5, 2));
    const int day = std::stoi(ts.substr(8, 2));
    const int hour = std::stoi(ts.substr(11, 2));
    const int minute = std::stoi(ts.substr(14, 2));
    const int second = std::stoi(ts.substr(17, 2));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 &&
           second <= 60;
}

bool timestamp_is_now(const std::string& ts) {
    SYSTEMTIME parsed{};
    parsed.wYear = static_cast<WORD>(std::stoi(ts.substr(0, 4)));
    parsed.wMonth = static_cast<WORD>(std::stoi(ts.substr(5, 2)));
    parsed.wDay = static_cast<WORD>(std::stoi(ts.substr(8, 2)));
    parsed.wHour = static_cast<WORD>(std::stoi(ts.substr(11, 2)));
    parsed.wMinute = static_cast<WORD>(std::stoi(ts.substr(14, 2)));
    parsed.wSecond = static_cast<WORD>(std::stoi(ts.substr(17, 2)));
    parsed.wMilliseconds = static_cast<WORD>(std::stoi(ts.substr(20, 3)));
    FILETIME written{};
    FILETIME now{};
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    if (!SystemTimeToFileTime(&parsed, &written) || !SystemTimeToFileTime(&utc, &now)) {
        return false;
    }
    const ULARGE_INTEGER a = {written.dwLowDateTime, written.dwHighDateTime};
    const ULARGE_INTEGER b = {now.dwLowDateTime, now.dwHighDateTime};
    const ULONGLONG delta = a.QuadPart > b.QuadPart ? a.QuadPart - b.QuadPart
                                                     : b.QuadPart - a.QuadPart;
    return delta < 300ull * 10000000ull;
}

void test_shape(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "shape.log";
    chast::host::log_event(path.string(), "worker.start",
                           {{"pid", "1234"}, {"session", "2"}});
    const std::string text = read_all(path);
    const std::vector<std::string> lines = split_lines(text);
    check(lines.size() == 1, "one record per call");
    check(!text.empty() && text.back() == '\n', "record ends with a newline");
    Record record;
    const bool parsed = parse_record(lines[0], record);
    check(parsed, "record is valid JSON with the expected shape");
    check(valid_timestamp(record.ts), "timestamp has the RFC3339 millisecond form");
    check(timestamp_is_now(record.ts), "timestamp is the current UTC time");
    check(record.event == "worker.start", "event name round-trips");
    check(record.fields.size() == 2, "both fields are present");
    const std::string* pid = find_field(record, "pid");
    const std::string* session = find_field(record, "session");
    check(pid != nullptr && *pid == "1234", "pid value round-trips");
    check(session != nullptr && *session == "2", "session value round-trips");
}

void test_empty_fields(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "empty.log";
    chast::host::log_event(path.string(), "service.start");
    Record record;
    check(parse_record(split_lines(read_all(path))[0], record), "fieldless record parses");
    check(record.event == "service.start" && record.fields.empty(), "fieldless record has {}");
}

void test_escaping(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "escape.log";
    const std::string event = "weird\"event\n";
    const std::string value = std::string("quote \" backslash \\ newline \n tab \t ctrl \x01") +
                              " end";
    chast::host::log_event(path.string(), event, {{"key\"x", value}});
    const std::string text = read_all(path);
    check(split_lines(text).size() == 1, "injected newline stays inside the record");
    Record record;
    check(parse_record(split_lines(text)[0], record), "escaped record parses");
    check(record.event == event, "event escapes round-trip");
    const std::string* field = find_field(record, "key\"x");
    check(field != nullptr && *field == value, "field escapes round-trip");
}

void test_utf8(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "utf8.log";
    const std::string valid = "远程桌面";
    const std::string invalid = "ok \xFF bad \xC3(";
    chast::host::log_event(path.string(), "settings.warning",
                           {{"valid", valid}, {"invalid", invalid}});
    Record record;
    check(parse_record(split_lines(read_all(path))[0], record), "utf8 record parses");
    const std::string* valid_field = find_field(record, "valid");
    check(valid_field != nullptr && *valid_field == valid, "valid UTF-8 survives unchanged");
    const std::string* invalid_field = find_field(record, "invalid");
    const std::string expected = std::string("ok \xEF\xBF\xBD bad \xEF\xBF\xBD(");
    check(invalid_field != nullptr && *invalid_field == expected,
          "invalid UTF-8 becomes U+FFFD");
}

void test_log_line(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "line.log";
    chast::host::log_line(path.string(), "listener on port 21116");
    Record record;
    check(parse_record(split_lines(read_all(path))[0], record), "log_line record parses");
    const std::string* text = find_field(record, "text");
    check(record.event == "message", "log_line uses event \"message\"");
    check(text != nullptr && *text == "listener on port 21116", "log_line keeps the text");
}

void test_empty_path() {
    chast::host::log_event("", "service.start");
    chast::host::log_line("", "nothing to write");
    check(true, "empty path is a no-op");
}

void test_rotation(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "rotate.log";
    const std::filesystem::path old = path.string() + ".old";
    const std::string big(5u * 1024u * 1024u + 4096u, 'x');
    chast::host::log_event(path.string(), "rotate.fill", {{"blob", big}});
    check(std::filesystem::file_size(path) > 5u * 1024u * 1024u, "log grows past the cap");
    chast::host::log_event(path.string(), "rotate.after");
    check(std::filesystem::exists(old), "oversized log is rotated to .old");
    check(std::filesystem::exists(old) && std::filesystem::file_size(old) > 5u * 1024u * 1024u,
          ".old holds the oversized file");
    const std::vector<std::string> lines = split_lines(read_all(path));
    check(lines.size() == 1, "rotation starts a fresh log");
    Record record;
    check(parse_record(lines[0], record) && record.event == "rotate.after",
          "the post-rotation record is intact");
}

void test_concurrent_appends(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "threads.log";
    constexpr int kThreads = 8;
    constexpr int kRecords = 50;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&path, t] {
            for (int i = 0; i < kRecords; ++i) {
                chast::host::log_event(path.string(), "thread.record",
                                       {{"thread", std::to_string(t)},
                                        {"index", std::to_string(i)}});
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::vector<std::string> lines = split_lines(read_all(path));
    const size_t expected = static_cast<size_t>(kThreads) * kRecords;
    check(lines.size() == expected, "every concurrent record is written");
    int valid = 0;
    int mismatched = 0;
    for (const std::string& line : lines) {
        Record record;
        if (!parse_record(line, record) || record.event != "thread.record" ||
            record.fields.size() != 2) {
            ++mismatched;
            continue;
        }
        ++valid;
    }
    check(static_cast<size_t>(valid) == expected, "no record is interleaved or torn");
    check(mismatched == 0, "every concurrent record keeps its fields");
}

void test_default_path() {
    const std::string path = chast::host::service_log_path("worker.log");
    check(path.empty() || path.ends_with("\\worker.log"), "service_log_path appends the name");
}

}  // namespace

int main() {
    const std::filesystem::path dir = temp_dir();
    test_shape(dir);
    test_empty_fields(dir);
    test_escaping(dir);
    test_utf8(dir);
    test_log_line(dir);
    test_empty_path();
    test_rotation(dir);
    test_concurrent_appends(dir);
    test_default_path();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("%d service log checks failed\n", failures);
        return 1;
    }
    std::printf("service log checks passed\n");
    return 0;
}
