#pragma once

// Host-side inbound audit spool.
//
// The host appends one JSON object per completed session or received file to
// a JSONL file. chast-tunnel host tails the file, signs each record with the
// device key and forwards it to the rendezvous service, so an audit record
// exists even when the viewer's own report never made it. The host does not
// put its device id or key in the file: the agent supplies both, which keeps a
// copied spool from being replayed against the original id.

#include <cstdint>
#include <string>

namespace chast {

struct AuditSessionRecord {
    // Lowercase hex from the viewer's Hello. Empty means the viewer did not
    // send one, and the record is dropped because the service cannot merge it.
    std::string audit_ref;
    int64_t started_at_ms = 0;
    int64_t ended_at_ms = 0;
    bool success = true;
    std::string detail;
};

struct AuditFileRecord {
    std::string audit_ref;
    // "upload" for a viewer -> host file.
    std::string direction;
    std::string name;
    uint64_t size = 0;
    int64_t occurred_at_ms = 0;
    bool success = true;
    std::string detail;
};

// Appends one line. Opens and closes the file per record so the agent can
// delete it between writes; missing directories are the caller's problem (the
// default spool sits next to host.ini, which already exists).
bool append_audit_session(const std::string& spool_path, const AuditSessionRecord& record,
                          std::string* error = nullptr);
bool append_audit_file(const std::string& spool_path, const AuditFileRecord& record,
                       std::string* error = nullptr);

}  // namespace chast
