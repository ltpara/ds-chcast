#pragma once

#include "portable_helper_input_journal.h"

#include <memory>
#include <mutex>
#include <string>

namespace chast::host {

class PortableHelperInputJournalFileStore;

namespace detail {
std::unique_ptr<PortableHelperInputJournalFileStore>
open_portable_helper_input_journal_file_store_for_test(
    const PortableHelperInputJournalScope& scope,
    const std::wstring& trusted_instance_directory, std::string* error);
bool remove_released_portable_helper_input_journals_for_test(
    const PortableHelperInstanceId& instance_id,
    const std::wstring& trusted_instance_directory, std::string* error);
}

// Durable Windows file backend for one instance/connection/session/generation
// journal. Production paths are derived from the protected ProgramData helper
// instance directory; no caller-supplied final or temporary filename is used.
class PortableHelperInputJournalFileStore final {
 public:
  static std::unique_ptr<PortableHelperInputJournalFileStore> OpenForInstance(
      const PortableHelperInputJournalScope& scope, std::string* error);
  // Removes all journal artifacts for an instance only after every durable
  // record is a released tombstone. Intended for final helper-instance
  // teardown, after the service and its workers have stopped.
  static bool RemoveReleasedForInstance(
      const PortableHelperInstanceId& instance_id, std::string* error);
  ~PortableHelperInputJournalFileStore();

  PortableHelperInputJournalFileStore(
      const PortableHelperInputJournalFileStore&) = delete;
  PortableHelperInputJournalFileStore& operator=(
      const PortableHelperInputJournalFileStore&) = delete;

  // Returns found=false only when no journal exists. Existing records are
  // opened no-follow and checked for protected ACL, regular-file type, a
  // single hard link, bounded size, strict encoding, and exact scope.
  bool Load(PortableHelperInputJournalImage* image, bool* found,
            std::string* error);

  // After Load found a record and the journal core accepted it through
  // Restore, enables recovery-only writes (normally MarkReleased). A loaded
  // record cannot be mistaken for a fresh Initialize image.
  bool ResumeLoadedRecord(const PortableHelperInputJournal& recovered_journal,
                          std::string* error);

  // Initialize uses an atomic no-replace install. Later transitions use a
  // flushed same-directory temporary file and write-through atomic replacement.
  // An existing record is never treated as a fresh empty generation.
  bool Persist(const PortableHelperInputJournalImage& image,
               std::string* error);

  const std::wstring& record_path() const noexcept { return record_path_; }

 private:
  friend std::unique_ptr<PortableHelperInputJournalFileStore>
  detail::open_portable_helper_input_journal_file_store_for_test(
      const PortableHelperInputJournalScope& scope,
      const std::wstring& trusted_instance_directory, std::string* error);
  friend bool detail::remove_released_portable_helper_input_journals_for_test(
      const PortableHelperInstanceId& instance_id,
      const std::wstring& trusted_instance_directory, std::string* error);

  PortableHelperInputJournalFileStore(
      PortableHelperInputJournalScope scope, std::wstring directory,
      std::wstring stem, void* lock_file);

  bool LoadLocked(PortableHelperInputJournalImage* image, bool* found,
                  std::string* error);
  bool PersistLocked(const PortableHelperInputJournalImage& image,
                     std::string* error);
  static std::unique_ptr<PortableHelperInputJournalFileStore>
  OpenAtDirectory(const PortableHelperInputJournalScope& scope,
                  const std::wstring& directory, std::string* error);
  static bool RemoveReleasedAtDirectory(
      const PortableHelperInstanceId& instance_id,
      const std::wstring& directory, std::string* error);

  PortableHelperInputJournalScope scope_;
  std::wstring directory_;
  std::wstring stem_;
  std::wstring record_path_;
  std::wstring temporary_path_;
  void* lock_file_ = nullptr;
  std::mutex mutex_;
  bool record_loaded_ = false;
  bool updates_enabled_ = false;
};

namespace detail {

// Test seam only: still requires an existing machine-state-trusted directory,
// but avoids writing to ProgramData during isolated filesystem regression.
std::unique_ptr<PortableHelperInputJournalFileStore>
open_portable_helper_input_journal_file_store_for_test(
    const PortableHelperInputJournalScope& scope,
    const std::wstring& trusted_instance_directory, std::string* error);

}  // namespace detail
}  // namespace chast::host
