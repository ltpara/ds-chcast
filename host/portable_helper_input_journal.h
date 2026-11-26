#pragma once

#include "portable_helper_authorization.h"
#include "portable_helper_protocol.h"
#include "chast/input.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace chast::host {

struct PortableHelperInputJournalScope final {
  PortableHelperInstanceId instance_id{};
  PortableHelperConnectionId connection_id{};
  uint32_t windows_session_id = 0;
  uint64_t generation = 0;
};

struct PortableHelperJournalKey final {
  uint16_t virtual_key = 0;
  uint16_t scan_code = 0;
  uint32_t flags = 0;
  uint32_t time = 0;
  uint64_t extra_info = 0;
};

struct PortableHelperJournalTouch final {
  uint32_t pointer_id = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint16_t pressure = 0;
};

struct PortableHelperJournalPen final {
  int32_t x = 0;
  int32_t y = 0;
  uint16_t pressure = 0;
  uint8_t flags = 0;
  int16_t tilt_x = 0;
  int16_t tilt_y = 0;
};

struct PortableHelperInputJournalState final {
  std::vector<PortableHelperJournalKey> keys;
  uint16_t mouse_buttons = 0;
  std::vector<PortableHelperJournalTouch> touches;
  bool pen_down = false;
  PortableHelperJournalPen pen;
};

// The persisted image contains both the last committed state and a
// conservative state for crash recovery. While pending is true, recovery must
// release from `possible`, the union of the before/after states.
struct PortableHelperInputJournalImage final {
  PortableHelperInputJournalScope scope;
  uint64_t last_sequence = 0;
  bool pending = false;
  bool released = false;
  uint64_t pending_sequence = 0;
  chast::InputSideEffectApi pending_api = chast::InputSideEffectApi::SendInput;
  PortableHelperInputJournalState committed;
  PortableHelperInputJournalState possible;
};

bool encode_portable_helper_input_journal(
    const PortableHelperInputJournalImage& image,
    std::vector<uint8_t>* encoded, std::string* error);
bool decode_portable_helper_input_journal(
    const std::vector<uint8_t>& encoded,
    PortableHelperInputJournalImage* image, std::string* error);

// A synchronous durability boundary. Production implementations must atomically
// persist each supplied image (including flushing it before returning true).
// Tests may inject an in-memory store to exercise state transitions without
// touching desktop input or privileged machine-state paths.
class PortableHelperInputJournal final {
 public:
  using Persist = std::function<bool(
      const PortableHelperInputJournalImage&, std::string*)>;

  PortableHelperInputJournal(PortableHelperInputJournalScope scope,
                             Persist persist);
  PortableHelperInputJournal(const PortableHelperInputJournal&) = delete;
  PortableHelperInputJournal& operator=(
      const PortableHelperInputJournal&) = delete;

  // Must atomically create a new durable record (never overwrite an existing
  // recovery record) before the helper starts the worker.
  bool Initialize(std::string* error);

  // Restores a previously decoded image for cleanup/recovery only. The same
  // generation never resumes input, even when its last image was committed;
  // Prepare remains blocked until cleanup is confirmed and MarkReleased makes
  // this journal terminal.
  bool Restore(const PortableHelperInputJournalImage& image,
               std::string* error);

  // Persists a write-ahead possible-state union before allowing the OS call.
  // Stateless mouse motion/wheel and Unicode text do not require disk writes.
  bool Prepare(const chast::InputSideEffectIntent& intent,
               std::string* error);

  // Commits a state transition only after the OS result is known and its new
  // image is durable. On persistence failure, the prior pending image remains
  // available for conservative recovery and this returns false.
  bool Complete(const chast::InputSideEffectResult& result,
                std::string* error);

  // Only after an independent session cleanup executor confirms release,
  // persist an empty state and make this journal instance terminal. This does
  // not remove the backing file; lifecycle code owns deletion after its
  // cleanup transaction succeeds. It may resolve an in-memory or recovered
  // pending intent because the external cleanup proof supersedes that result.
  bool MarkReleased(std::string* error);

  PortableHelperInputJournalImage image() const;
  bool has_pending_operation() const;
  bool recovery_only() const;

 private:
  struct PendingOperation final {
    uint64_t sequence = 0;
    chast::InputSideEffectApi api = chast::InputSideEffectApi::SendInput;
    bool stateful = false;
    PortableHelperInputJournalState candidate;
  };

  PortableHelperInputJournalScope scope_;
  Persist persist_;
  PortableHelperInputJournalImage image_;
  PendingOperation pending_;
  bool initialized_ = false;
  bool terminal_ = false;
  bool recovery_blocked_ = false;
};

}  // namespace chast::host
