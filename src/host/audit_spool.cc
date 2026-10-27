#include "chast/audit_spool.h"

#include <windows.h>

#include <cstdio>

namespace chast {

namespace {

std::wstring wide_path(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

void append_json_string(std::string& out, const std::string& value) {
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
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
                if (c < 0x20) {
                    char escaped[7]{};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, const char* key, int64_t value) {
    out += ",\"";
    out += key;
    out += "\":";
    out += std::to_string(value);
}

void append_bool(std::string& out, const char* key, bool value) {
    out += ",\"";
    out += key;
    out += "\":";
    out += value ? "true" : "false";
}

bool append_line(const std::string& spool_path, const std::string& line, std::string* error) {
    if (spool_path.empty()) {
        if (error != nullptr) {
            *error = "audit spool path is empty";
        }
        return false;
    }
    const std::wstring path = wide_path(spool_path);
    if (path.empty()) {
        if (error != nullptr) {
            *error = "audit spool path is not UTF-8";
        }
        return false;
    }
    // Deliberately no FILE_SHARE_DELETE: the agent removes the spool after
    // storing every line, and a delete in the middle of this append would
    // throw the new record away. The agent retries on the next tick.
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "cannot open audit spool " + spool_path + " (Windows error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    DWORD written = 0;
    const BOOL ok =
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    const DWORD write_error = ok ? 0 : GetLastError();
    CloseHandle(file);
    if (!ok || written != line.size()) {
        if (error != nullptr) {
            *error = "cannot write audit spool " + spool_path + " (Windows error " +
                     std::to_string(write_error) + ")";
        }
        return false;
    }
    return true;
}

}  // namespace

bool append_audit_session(const std::string& spool_path, const AuditSessionRecord& record,
                          std::string* error) {
    if (record.audit_ref.empty()) {
        if (error != nullptr) {
            *error = "viewer sent no audit reference";
        }
        return false;
    }
    std::string line = "{\"kind\":\"session\",\"audit_ref\":";
    append_json_string(line, record.audit_ref);
    append_number(line, "started_at_ms", record.started_at_ms);
    append_number(line, "ended_at_ms", record.ended_at_ms);
    append_bool(line, "success", record.success);
    line += ",\"detail\":";
    append_json_string(line, record.detail);
    line += "}\n";
    return append_line(spool_path, line, error);
}

bool append_audit_file(const std::string& spool_path, const AuditFileRecord& record,
                       std::string* error) {
    if (record.audit_ref.empty()) {
        if (error != nullptr) {
            *error = "viewer sent no audit reference";
        }
        return false;
    }
    std::string line = "{\"kind\":\"file\",\"audit_ref\":";
    append_json_string(line, record.audit_ref);
    line += ",\"direction\":";
    append_json_string(line, record.direction);
    line += ",\"name\":";
    append_json_string(line, record.name);
    line += ",\"size\":" + std::to_string(record.size);
    append_number(line, "occurred_at_ms", record.occurred_at_ms);
    append_bool(line, "success", record.success);
    line += ",\"detail\":";
    append_json_string(line, record.detail);
    line += "}\n";
    return append_line(spool_path, line, error);
}

}  // namespace chast
