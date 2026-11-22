#include <windows.h>
#include <flutter_windows.h>

#include <cstdio>
#include <filesystem>

#include "flutter_view_initialization.h"

namespace {
HHOOK query_hook = nullptr;
unsigned queries = 0;

// Exercise the timing of an accessibility client querying a new HWND before
// Flutter assigns its view delegate. This is a local test window, not UI input.
LRESULT CALLBACK QueryDuringCreation(int code, WPARAM wparam, LPARAM lparam) {
  if (code >= 0) {
    const auto* message = reinterpret_cast<CWPRETSTRUCT*>(lparam);
    wchar_t name[64]{};
    if (message->message == WM_NCCREATE &&
        GetClassNameW(message->hwnd, name, 64) &&
        wcscmp(name, L"FLUTTERVIEW") == 0) {
      ++queries;
      SendMessageW(message->hwnd, WM_GETOBJECT, 0,
                   static_cast<LPARAM>(OBJID_CLIENT));
    }
  }
  return CallNextHookEx(query_hook, code, wparam, lparam);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::fputs("usage: chast-flutter-view-initialization-test <release-data>\n", stderr);
    return 2;
  }
  const auto data = std::filesystem::absolute(argv[1]);
  const auto assets = (data / L"flutter_assets").wstring();
  const auto icu = (data / L"icudtl.dat").wstring();
  const auto aot = (data / L"app.so").wstring();
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) return 2;
  query_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, QueryDuringCreation,
                                nullptr, GetCurrentThreadId());
  if (!query_hook) { CoUninitialize(); return 2; }
  FlutterDesktopViewControllerRef controllers[2]{};
  bool ok = true;
  for (auto& controller : controllers) {
    FlutterDesktopEngineProperties properties{};
    properties.assets_path = assets.c_str();
    properties.icu_data_path = icu.c_str();
    properties.aot_library_path = aot.c_str();
    auto engine = FlutterDesktopEngineCreate(&properties);
    if (!engine) { ok = false; break; }
    {
      ScopedFlutterViewInitialization initialization;
      if (!initialization.ready()) {
        FlutterDesktopEngineDestroy(engine);
        ok = false;
        break;
      }
      controller = FlutterDesktopViewControllerCreate(320, 240, engine);
    }
    if (!controller) { ok = false; break; }
    // The guard has been removed: accessibility requests must reach Flutter
    // normally after construction, while the other engine remains alive.
    const HWND window = FlutterDesktopViewGetHWND(
        FlutterDesktopViewControllerGetView(controller));
    SendMessageW(window, WM_GETOBJECT, 0, static_cast<LPARAM>(OBJID_CLIENT));
  }
  UnhookWindowsHookEx(query_hook);
  for (int i = 1; i >= 0; --i)
    if (controllers[i]) FlutterDesktopViewControllerDestroy(controllers[i]);
  CoUninitialize();
  if (!ok || queries != 2) {
    std::fprintf(stderr, "FAIL controller creation; early queries=%u\n", queries);
    return 1;
  }
  std::puts("PASS two Flutter engines: early accessibility queries guarded; post-construction queries and child-first destruction completed");
  return 0;
}
