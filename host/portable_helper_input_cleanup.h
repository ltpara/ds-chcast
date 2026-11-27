#pragma once

#include "portable_helper_input_journal_store.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace chast::host {

// An exact, bounded release set reconstructed from the journal's conservative
// `possible` image. It contains no keys or contacts absent from that scope.
struct PortableHelperInputCleanupPlan final {
  std::string input_desktop_name;
  std::vector<INPUT> mouse_buttons_up;
  std::vector<POINTER_TOUCH_INFO> touch_contacts_up;
  bool pen_up = false;
  PortableHelperJournalPen pen;
  std::vector<INPUT> keys_up;
};

bool make_portable_helper_input_cleanup_plan(
    const PortableHelperInputJournalScope& expected_scope,
    const PortableHelperInputJournalImage& image,
    PortableHelperInputCleanupPlan* plan, std::string* error);

// Executes the plan in the target session's current input desktop. The caller
// must already have authenticated the dedicated cleanup process and rechecked
// its live session identity. This function is bounded and reports success only
// when every requested release API confirms the complete operation.
bool execute_portable_helper_input_cleanup_plan(
    const PortableHelperInputCleanupPlan& plan, std::string* error);

// The callback is the process boundary: production callers must implement it
// by launching a separate, peer-authenticated SYSTEM process in the exact
// target session, and return true only after that process confirms all release
// APIs. Tests inject a fake executor. The journal is marked released only
// after this callback succeeds; a failed/lost response leaves it retryable.
using PortableHelperAuthenticatedInputCleanupExecutor = std::function<bool(
    const PortableHelperInputJournalScope&,
    const PortableHelperInputJournalImage&,
    std::string*)>;

bool recover_portable_helper_input_journal(
    const PortableHelperInputJournalScope& expected_scope,
    PortableHelperInputJournalFileStore& store,
    const PortableHelperAuthenticatedInputCleanupExecutor& executor,
    std::string* error);

}  // namespace chast::host
