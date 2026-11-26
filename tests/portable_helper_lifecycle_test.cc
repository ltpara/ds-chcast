#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "portable_helper_lifecycle.h"
#include "portable_helper_recovery.h"
#include "portable_helper_service_channel.h"
#include "portable_helper_staging.h"

namespace {

using namespace chast::host;
int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) ++failures;
}

PortableHelperServiceSpec test_spec() {
  PortableHelperInstanceId instance{};
  instance[0] = 0x42;
  PortableHelperProcessIdentity parent;
  parent.process_id = 100;
  parent.session_id = 1;
  parent.creation_time = 200;
  parent.image_path = L"C:\\Program Files\\Chast\\chast-host.exe";
  parent.user_sid = L"S-1-5-21-100-200-300-1001";
  parent.logon_sid = L"S-1-5-5-0-12345";
  std::wstring stage_directory, executable_path;
  std::string error;
  const bool got_paths = portable_helper_staging_paths(
      instance, &stage_directory, &executable_path, &error);
  check(got_paths, "resolve this instance's ProgramData staging paths");
  PortableHelperServiceSpec spec;
  check(make_portable_helper_service_spec(
            instance, executable_path,
            parent, &spec, &error),
        "build a service spec from a random instance and interactive parent identity");
  return spec;
}

struct FakeBackend final : PortableHelperServiceBackend {
  bool fail_create = false;
  bool fail_start = false;
  bool fail_connect = false;
  bool service_registered = false;
  bool simulate_name_collision = false;
  int fail_stop_count = 0;
  int fail_delete_count = 0;
  int fail_staged_cleanup_count = 0;
  int create_calls = 0;
  int start_calls = 0;
  int connect_calls = 0;
  int stop_calls = 0;
  int delete_calls = 0;
  int staged_cleanup_calls = 0;
  std::vector<std::string> events;

  bool CreateExclusive(const PortableHelperServiceSpec&, std::string* error) override {
    ++create_calls;
    events.push_back("create");
    if (!fail_create) {
      service_registered = true;
      return true;
    }
    service_registered = simulate_name_collision;
    if (error) *error = "name collision";
    return false;
  }

  bool Start(const PortableHelperServiceSpec&, std::string* error) override {
    ++start_calls;
    events.push_back("start");
    if (!fail_start) return true;
    if (error) *error = "start rejected";
    return false;
  }

  bool Connect(const PortableHelperServiceSpec&, uint32_t,
               std::unique_ptr<PortableHelperServiceConnection>* connection,
               std::string* error) override {
    ++connect_calls;
    events.push_back("connect");
    if (fail_connect) {
      if (error) *error = "control channel rejected";
      return false;
    }
    if (connection == nullptr) {
      if (error) *error = "missing connection output";
      return false;
    }
    *connection = std::make_unique<PortableHelperServiceConnection>();
    return true;
  }

  bool Stop(const PortableHelperServiceSpec&, std::string* error) override {
    ++stop_calls;
    events.push_back("stop");
    if (fail_stop_count-- > 0) {
      if (error) *error = "stop timed out";
      return false;
    }
    return true;
  }

  bool DeleteIfOwned(const PortableHelperServiceSpec&,
                     std::string* error) override {
    ++delete_calls;
    events.push_back("delete");
    if (fail_delete_count-- > 0) {
      if (error) *error = "delete pending";
      return false;
    }
    service_registered = false;
    return true;
  }

  bool RemoveStagedImage(const PortableHelperServiceSpec&,
                         std::string* error) override {
    ++staged_cleanup_calls;
    events.push_back("remove-image");
    if (service_registered) {
      if (error) *error = "colliding service still registered";
      return false;
    }
    if (fail_staged_cleanup_count-- > 0) {
      if (error) *error = "staged image cleanup pending";
      return false;
    }
    return true;
  }
};

void test_instance_names_and_spec_validation() {
  PortableHelperInstanceId zero{};
  check(portable_helper_service_name(zero).empty(),
        "an empty instance id cannot name a service");
  PortableHelperInstanceId generated{};
  std::string error;
  check(generate_portable_helper_instance_id(&generated, &error) &&
            std::any_of(generated.begin(), generated.end(),
                        [](uint8_t byte) { return byte != 0; }),
        "helper service instances use a cryptographically random nonzero ID");
  const auto spec = test_spec();
  check(spec.service_name == L"ChastPortableHelper-42000000000000000000000000000000" &&
            spec.service_name != L"chast-host",
        "temporary service names are stable per instance and separate from chast-host");
  std::wstring expected_directory, expected_executable;
  check(portable_helper_staging_paths(spec.instance_id, &expected_directory,
                                      &expected_executable, &error) &&
            spec.staging_directory == expected_directory &&
            spec.executable_path == expected_executable,
        "service image path is bound to its instance-specific ProgramData directory");

  auto invalid_parent = spec.parent;
  invalid_parent.session_id = 0;
  PortableHelperServiceSpec invalid;
  check(!make_portable_helper_service_spec(
            spec.instance_id, spec.executable_path, invalid_parent, &invalid,
            &error) && !error.empty(),
        "a non-interactive parent cannot create a desktop helper service spec");
  check(!make_portable_helper_service_spec(
            spec.instance_id, L"relative\\chast-helper.exe", spec.parent,
            &invalid, &error),
        "relative service image paths are rejected");
  check(!make_portable_helper_service_spec(
            spec.instance_id,
            L"C:\\ProgramData\\Chast\\chast-host.exe", spec.parent,
            &invalid, &error),
        "service image paths outside the per-instance staging folder are rejected");
}

void test_service_start_arguments_are_exact_and_instance_bound() {
  const auto expected = test_spec();
  const std::vector<std::wstring> valid_arguments = {
      L"--portable-helper-instance", L"42000000000000000000000000000000",
      L"--parent-pid", L"100",
      L"--parent-created", L"200",
      L"--parent-session", L"1",
      L"--parent-image", L"C:\\Program Files\\Chast\\chast-host.exe",
      L"--parent-user-sid", L"S-1-5-21-100-200-300-1001",
      L"--parent-logon-sid", L"S-1-5-5-0-12345",
  };
  PortableHelperServiceSpec parsed;
  std::string error;
  check(parse_portable_helper_service_start_arguments(
            expected.service_name, valid_arguments, expected.executable_path,
            &parsed, &error) &&
            parsed.instance_id == expected.instance_id &&
            parsed.parent.process_id == expected.parent.process_id &&
            parsed.parent.image_path == expected.parent.image_path,
        "SCM start arguments reconstruct the exact helper and parent identity");

  check(!parse_portable_helper_service_start_arguments(
            L"ChastPortableHelper-43000000000000000000000000000000",
            valid_arguments, expected.executable_path, &parsed, &error),
        "a service name for another instance cannot reuse these start arguments");

  auto malformed = valid_arguments;
  malformed[3] = L"-1";
  check(!parse_portable_helper_service_start_arguments(
            expected.service_name, malformed, expected.executable_path,
            &parsed, &error),
        "signed and non-decimal parent IDs are rejected");

  malformed = valid_arguments;
  malformed.push_back(L"--unexpected");
  check(!parse_portable_helper_service_start_arguments(
            expected.service_name, malformed, expected.executable_path,
            &parsed, &error),
        "extra SCM start arguments are rejected");

  malformed = valid_arguments;
  malformed[1] = L"4200000000000000000000000000000g";
  check(!parse_portable_helper_service_start_arguments(
            expected.service_name, malformed, expected.executable_path,
            &parsed, &error),
        "non-hexadecimal instance IDs are rejected");
}

void test_recovery_record_is_versioned_and_instance_bound() {
  const auto expected = test_spec();
  std::vector<uint8_t> encoded;
  std::string error;
  check(encode_portable_helper_recovery_record(expected, &encoded, &error) &&
            encoded.size() > 40,
        "encode a bounded helper recovery record for the staged instance");
  PortableHelperServiceSpec decoded;
  check(decode_portable_helper_recovery_record(encoded, &decoded, &error) &&
            decoded.instance_id == expected.instance_id &&
            decoded.service_name == expected.service_name &&
            decoded.executable_path == expected.executable_path &&
            decoded.parent.process_id == expected.parent.process_id &&
            decoded.parent.creation_time == expected.parent.creation_time &&
            decoded.parent.image_path == expected.parent.image_path &&
            decoded.parent.user_sid == expected.parent.user_sid &&
            decoded.parent.logon_sid == expected.parent.logon_sid,
        "recovery record restores the exact service, image and parent identity");

  auto malformed = encoded;
  malformed[0] ^= 0xff;
  check(!decode_portable_helper_recovery_record(malformed, &decoded, &error),
        "recovery record with a wrong magic is rejected");

  malformed = encoded;
  malformed.pop_back();
  check(!decode_portable_helper_recovery_record(malformed, &decoded, &error),
        "truncated recovery record is rejected");

  malformed = encoded;
  malformed.push_back(0);
  check(!decode_portable_helper_recovery_record(malformed, &decoded, &error),
        "recovery record trailing bytes are rejected");

  auto invalid = expected;
  invalid.parent.process_id = 0;
  check(!encode_portable_helper_recovery_record(invalid, &malformed, &error),
        "non-canonical service identity cannot be persisted");
}

void test_publisher_signature_requires_trusted_pinned_signer() {
  std::string error;
  check(!verify_portable_helper_publisher_signature(
            L"C:\\missing\\chast-host.exe", {}, &error) &&
            error.find("thumbprint") != std::string::npos,
        "publisher verification rejects a missing certificate pin");
  check(!verify_portable_helper_publisher_signature(
            L"C:\\missing\\chast-host.exe", "not-a-thumbprint", &error) &&
            error.find("hexadecimal") != std::string::npos,
        "publisher verification rejects malformed certificate pins");
  wchar_t image[MAX_PATH * 2]{};
  const DWORD image_length = GetModuleFileNameW(
      nullptr, image, static_cast<DWORD>(std::size(image)));
  check(image_length != 0 && image_length < std::size(image),
        "resolve the unsigned lifecycle test executable for signature rejection");
  if (image_length != 0 && image_length < std::size(image)) {
    check(!verify_portable_helper_publisher_signature(
              std::wstring(image, image_length), std::string(64, '0'), &error) &&
              error.find("Authenticode validation failed") != std::string::npos,
          "an unsigned executable is rejected even when a publisher pin is configured");

    error.clear();
    const bool configured_signature_ok =
        verify_configured_portable_helper_publisher_signature(
            std::wstring(image, image_length), &error);
    if (!configured_signature_ok &&
        error.find("staging is disabled") != std::string::npos) {
      PortableHelperServiceSpec spec;
      spec.service_name = L"must-be-cleared";
      error.clear();
      check(!stage_portable_helper_service_image(&spec, &error) &&
                spec.service_name.empty() &&
                error.find("staging is disabled") != std::string::npos,
            "an unset publisher pin rejects staging before elevation or filesystem access");
    } else {
      check(configured_signature_ok ||
                error.find("Authenticode validation failed") != std::string::npos,
            "configured publisher verification accepts only a trusted pinned signature");
    }
  }
}

struct TempDirectory {
  std::filesystem::path path;

  TempDirectory() {
    wchar_t temp_path[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temp_path)),
                                      temp_path);
    if (length == 0 || length >= std::size(temp_path)) return;
    path = std::filesystem::path(temp_path) /
        (L"chast-helper-copy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::error_code error;
    if (!std::filesystem::create_directory(path, error) || error) path.clear();
  }

  ~TempDirectory() {
    if (path.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

bool files_are_equal(const std::filesystem::path& left,
                     const std::filesystem::path& right) {
  std::ifstream a(left, std::ios::binary);
  std::ifstream b(right, std::ios::binary);
  if (!a || !b) return false;
  std::array<char, 64 * 1024> left_bytes{}, right_bytes{};
  for (;;) {
    a.read(left_bytes.data(), static_cast<std::streamsize>(left_bytes.size()));
    b.read(right_bytes.data(), static_cast<std::streamsize>(right_bytes.size()));
    const auto left_count = a.gcount();
    const auto right_count = b.gcount();
    if (left_count != right_count ||
        !std::equal(left_bytes.begin(), left_bytes.begin() + left_count,
                    right_bytes.begin()))
      return false;
    if (left_count == 0) return a.eof() && b.eof();
    if (a.bad() || b.bad()) return false;
  }
}

std::wstring current_test_image() {
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                         static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return {};
  return std::wstring(path.data(), length);
}

void test_helper_image_copy_is_validated_and_exclusive() {
  TempDirectory temp;
  check(!temp.path.empty(), "create a private temp directory for helper copy tests");
  if (temp.path.empty()) return;

  const std::wstring source = current_test_image();
  const auto copy = temp.path / L"helper-copy.exe";
  std::string error;
  check(!source.empty() && detail::copy_portable_helper_image_file(
            source, copy.wstring(), &error) && files_are_equal(source, copy),
        "copy a validated PE image byte-for-byte into an exclusive target");

  const auto existing = temp.path / L"existing.exe";
  {
    std::ofstream file(existing, std::ios::binary);
    file << "keep-existing-image";
  }
  std::string existing_contents;
  {
    std::ifstream file(existing, std::ios::binary);
    existing_contents.assign(std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>());
  }
  check(!detail::copy_portable_helper_image_file(
            source, existing.wstring(), &error) &&
            std::filesystem::file_size(existing) == 19 &&
            existing_contents == "keep-existing-image",
        "an existing staging target is never overwritten");

  const auto invalid_source = temp.path / L"not-an-image.bin";
  {
    std::ofstream file(invalid_source, std::ios::binary);
    file << "not a PE image";
  }
  const auto rejected_target = temp.path / L"rejected.exe";
  check(!detail::copy_portable_helper_image_file(
            invalid_source.wstring(), rejected_target.wstring(), &error) &&
            !std::filesystem::exists(rejected_target),
        "a non-PE source is rejected before any target is created");

  if (!source.empty()) {
    const auto hard_link = source + L".helper-copy-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const auto hard_link_target = temp.path / L"hard-link-copy.exe";
    if (CreateHardLinkW(hard_link.c_str(), source.c_str(), nullptr)) {
      const bool rejected = !detail::copy_portable_helper_image_file(
          hard_link, hard_link_target.wstring(), &error) &&
          !std::filesystem::exists(hard_link_target);
      const bool removed = DeleteFileW(hard_link.c_str()) != FALSE;
      check(rejected && removed,
            "a hard-linked helper source is rejected without leaving a copy");
    } else {
      std::printf("skip hard-link helper copy check (Win32 error %lu)\n",
                  GetLastError());
    }
  }

}

void test_successful_lifecycle_is_ordered_and_idempotent() {
  FakeBackend backend;
  PortableHelperServiceLifecycle lifecycle(test_spec(), backend);
  std::string error;
  check(lifecycle.CreateAndStart(&error) &&
            lifecycle.state() == PortableHelperServiceState::Running,
        "a new helper service is started and its control channel authenticated");
  check(lifecycle.CreateAndStart(&error) && backend.create_calls == 1 &&
            backend.start_calls == 1 && backend.connect_calls == 1,
        "repeated start does not create a second service instance");
  check(lifecycle.Stop(&error) &&
            lifecycle.state() == PortableHelperServiceState::Created,
        "stop leaves the owned service created and restartable");
  check(lifecycle.CreateAndStart(&error) && backend.create_calls == 1 &&
            backend.start_calls == 2 && backend.connect_calls == 2,
        "a stopped temporary service can be restarted without recreation");
  check(lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::Removed,
        "cleanup stops and removes the exact temporary service");
  const auto calls = backend.events.size();
  check(lifecycle.Cleanup(&error) && backend.events.size() == calls,
        "repeated cleanup after removal is idempotent");
  check(backend.events == std::vector<std::string>{
            "create", "start", "connect", "stop", "start", "connect",
            "stop", "delete",
            "remove-image"},
        "lifecycle authenticates IPC and stops/deletes the service before removing its image");
}

void test_start_failure_attempts_stop_and_delete() {
  FakeBackend backend;
  backend.fail_start = true;
  PortableHelperServiceLifecycle lifecycle(test_spec(), backend);
  std::string error;
  check(!lifecycle.CreateAndStart(&error) &&
            error.find("start rejected") != std::string::npos,
        "a failed start remains a reported operation failure");
  check(lifecycle.state() == PortableHelperServiceState::Removed &&
            backend.stop_calls == 1 && backend.delete_calls == 1 &&
            backend.staged_cleanup_calls == 1,
        "a failed start is followed by stop, service deletion and image cleanup");
}

void test_control_channel_failure_rolls_back_service_start() {
  FakeBackend backend;
  backend.fail_connect = true;
  PortableHelperServiceLifecycle lifecycle(test_spec(), backend);
  std::string error;
  check(!lifecycle.CreateAndStart(&error) &&
            error.find("control channel rejected") != std::string::npos,
        "an unauthenticated service control channel fails startup");
  check(lifecycle.state() == PortableHelperServiceState::Removed &&
            backend.stop_calls == 1 && backend.delete_calls == 1 &&
            backend.staged_cleanup_calls == 1,
        "control-channel failure stops and removes only the newly owned instance");
}

void test_stop_failure_prevents_delete_and_can_be_retried() {
  FakeBackend backend;
  backend.fail_stop_count = 1;
  PortableHelperServiceLifecycle lifecycle(test_spec(), backend);
  std::string error;
  check(lifecycle.CreateAndStart(&error), "start the helper before stop-failure coverage");
  check(!lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::CleanupPending &&
            backend.delete_calls == 0 && backend.staged_cleanup_calls == 0,
        "a stop failure retains the service and prevents unsafe deletion");
  check(lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::Removed &&
            backend.stop_calls == 2 && backend.delete_calls == 1 &&
            backend.staged_cleanup_calls == 1,
        "cleanup retries the stop before deleting the service");
}

void test_delete_failure_is_visible_and_retryable() {
  FakeBackend backend;
  backend.fail_delete_count = 1;
  PortableHelperServiceLifecycle lifecycle(test_spec(), backend);
  std::string error;
  check(lifecycle.CreateAndStart(&error), "start the helper before delete-failure coverage");
  check(!lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::CleanupPending &&
            error.find("delete pending") != std::string::npos &&
            backend.staged_cleanup_calls == 0,
        "a failed SCM deletion is exposed as cleanup pending");
  check(lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::Removed &&
            backend.stop_calls == 1 && backend.delete_calls == 2 &&
            backend.staged_cleanup_calls == 1,
        "retry removes a pending service before cleaning its image");
}

void test_staged_image_cleanup_failure_is_visible_and_retryable() {
  FakeBackend backend;
  backend.fail_staged_cleanup_count = 1;
  PortableHelperServiceLifecycle lifecycle(test_spec(), backend);
  std::string error;
  check(lifecycle.CreateAndStart(&error),
        "start the helper before image-cleanup failure coverage");
  check(!lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::CleanupPending &&
            error.find("staged image cleanup pending") != std::string::npos &&
            backend.delete_calls == 1 && backend.staged_cleanup_calls == 1,
        "a staged-image cleanup failure remains visible after service deletion");
  check(lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::Removed &&
            backend.stop_calls == 1 && backend.delete_calls == 2 &&
            backend.staged_cleanup_calls == 2,
        "retry confirms service deletion and retries only the pending image cleanup");
}

void test_recovery_scan_keeps_live_parent_and_cleans_only_stale_records() {
  const auto active = test_spec();
  auto build_spec = [&](uint8_t id_byte, uint32_t process_id,
                        PortableHelperServiceSpec* result) {
    auto instance = active.instance_id;
    instance[0] = id_byte;
    auto parent = active.parent;
    parent.process_id = process_id;
    std::wstring directory, image;
    std::string ignored;
    if (!portable_helper_staging_paths(instance, &directory, &image, &ignored))
      return false;
    return make_portable_helper_service_spec(instance, image, parent,
                                             result, &ignored);
  };
  PortableHelperServiceSpec stale, unresolved;
  check(build_spec(0x43, 101, &stale) &&
            build_spec(0x44, 102, &unresolved),
        "build distinct recovery candidates with valid instance identities");
  if (stale.service_name.empty() || unresolved.service_name.empty()) return;

  FakeBackend backend;
  PortableHelperRecoveryReport report;
  std::string error;
  const bool recovered = recover_portable_helper_records(
      {active, stale, unresolved},
      [](const PortableHelperProcessIdentity& parent, bool* alive,
         std::string* detail) {
        if (parent.process_id == 102) {
          if (detail) *detail = "parent identity query unavailable";
          return false;
        }
        if (alive) *alive = parent.process_id == 100;
        return true;
      },
      backend, &report, &error);
  check(recovered && report.inspected == 3 && report.active == 1 &&
            report.cleaned == 1 && report.pending == 1 && report.invalid == 0,
        "recovery retains an exact live parent, cleans an exited parent and reports uncertainty");
  check(backend.events == std::vector<std::string>{
            "stop", "delete", "remove-image"},
        "recovery cleanup stops and deletes only the stale record's exact service before its image");
}

void test_recovery_stops_after_current_backend_operation_when_cancelled() {
  FakeBackend backend;
  const auto base = test_spec();
  PortableHelperServiceSpec first, second, third;
  auto build_spec = [&](uint8_t id_byte, uint32_t process_id,
                        PortableHelperServiceSpec* result) {
    auto instance = base.instance_id;
    instance[0] = id_byte;
    auto parent = base.parent;
    parent.process_id = process_id;
    std::wstring directory, image;
    std::string ignored;
    return portable_helper_staging_paths(instance, &directory, &image, &ignored) &&
        make_portable_helper_service_spec(instance, image, parent, result, &ignored);
  };
  check(build_spec(0x51, 201, &first) && build_spec(0x52, 202, &second) &&
            build_spec(0x53, 203, &third),
        "build valid recovery candidates for cancellation coverage");
  PortableHelperRecoveryReport report;
  std::string error;
  check(recover_portable_helper_records(
            {first, second, third},
            [](const PortableHelperProcessIdentity&, bool* alive,
               std::string*) {
              *alive = false;
              return true;
            },
            backend, &report, &error,
            [] { return false; }),
        "cancel helper recovery without failing the enclosing host service");
  check(report.interrupted && report.inspected == 0 && report.cleaned == 0 &&
            backend.stop_calls == 0 && backend.delete_calls == 0 &&
            backend.staged_cleanup_calls == 0,
        "recovery observes cancellation before the first backend mutation");

  backend = FakeBackend{};
  size_t continue_checks = 0;
  check(recover_portable_helper_records(
            {first, second, third},
            [](const PortableHelperProcessIdentity&, bool* alive,
               std::string*) {
              *alive = false;
              return true;
            },
            backend, &report, &error,
            [&continue_checks] { return ++continue_checks < 2; }),
        "cancel a recovery pass after its first backend operation");
  check(report.interrupted && report.inspected == 1 && report.cleaned == 0 &&
            backend.stop_calls == 1 && backend.delete_calls == 0 &&
            backend.staged_cleanup_calls == 0,
        "recovery stops at a completed SCM operation and defers the remaining cleanup safely");
}

void test_create_failure_never_starts_or_deletes_a_collision() {
  FakeBackend backend;
  backend.fail_create = true;
  backend.simulate_name_collision = true;
  PortableHelperServiceLifecycle lifecycle(test_spec(), backend);
  std::string error;
  check(!lifecycle.CreateAndStart(&error) &&
            lifecycle.state() == PortableHelperServiceState::CleanupPending,
        "a service-name collision fails without claiming ownership");
  check(backend.start_calls == 0 && backend.stop_calls == 0 &&
            backend.delete_calls == 0 && backend.staged_cleanup_calls == 1,
        "a colliding service is never started, stopped, deleted or deprived of its image");
  backend.fail_create = false;
  backend.service_registered = false;
  check(lifecycle.Cleanup(&error) &&
            lifecycle.state() == PortableHelperServiceState::Removed &&
            backend.delete_calls == 0 && backend.staged_cleanup_calls == 2,
        "once the collision is gone, cleanup removes only the staged image");
}

void test_invalid_spec_never_reaches_the_service_backend() {
  auto spec = test_spec();
  spec.service_name = L"chast-host";
  FakeBackend backend;
  PortableHelperServiceLifecycle lifecycle(std::move(spec), backend);
  std::string error;
  check(!lifecycle.CreateAndStart(&error) &&
            lifecycle.state() == PortableHelperServiceState::NotCreated &&
            !error.empty(),
        "the installed service name is rejected as a helper instance name");
  check(backend.create_calls == 0 && backend.start_calls == 0 &&
            backend.stop_calls == 0 && backend.delete_calls == 0,
        "an invalid service spec cannot reach any backend operation");
}

}  // namespace

int main() {
  test_instance_names_and_spec_validation();
  test_service_start_arguments_are_exact_and_instance_bound();
  test_recovery_record_is_versioned_and_instance_bound();
  test_publisher_signature_requires_trusted_pinned_signer();
  test_helper_image_copy_is_validated_and_exclusive();
  test_successful_lifecycle_is_ordered_and_idempotent();
  test_start_failure_attempts_stop_and_delete();
  test_control_channel_failure_rolls_back_service_start();
  test_stop_failure_prevents_delete_and_can_be_retried();
  test_delete_failure_is_visible_and_retryable();
  test_staged_image_cleanup_failure_is_visible_and_retryable();
  test_recovery_scan_keeps_live_parent_and_cleans_only_stale_records();
  test_recovery_stops_after_current_backend_operation_when_cancelled();
  test_create_failure_never_starts_or_deletes_a_collision();
  test_invalid_spec_never_reaches_the_service_backend();
  return failures == 0 ? 0 : 1;
}
