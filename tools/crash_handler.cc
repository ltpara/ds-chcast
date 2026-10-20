#include "crash_handler.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace chast_tools {

namespace {

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        default: return "exception";
    }
}

LONG WINAPI filter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    std::fprintf(stderr, "\n*** %s (0x%08lX) at %p",
                 exception_name(record->ExceptionCode),
                 static_cast<unsigned long>(record->ExceptionCode), record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR operation = record->ExceptionInformation[0];
        std::fprintf(stderr, " %s address %p", operation == 0 ? "reading" : "writing",
                     reinterpret_cast<void*>(record->ExceptionInformation[1]));
    }
    std::fprintf(stderr, "\n");

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    // Look for PDBs next to the executable so local builds symbolize.
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    std::string search_path(module_path);
    const size_t slash = search_path.find_last_of("\\/");
    if (slash != std::string::npos) {
        search_path.resize(slash);
    }
    SymInitialize(process, search_path.c_str(), TRUE);

    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int depth = 0; depth < 32; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }

        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;

        char module_name[MAX_PATH] = "?";
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        if (SymGetModuleInfo64(process, frame.AddrPC.Offset, &module)) {
            std::snprintf(module_name, sizeof(module_name), "%s", module.ModuleName);
        }

        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
            DWORD line_displacement = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line)) {
                std::fprintf(stderr, "  %s!%s+0x%llX (%s:%lu)\n", module_name, symbol->Name,
                             static_cast<unsigned long long>(displacement), line.FileName,
                             line.LineNumber);
            } else {
                std::fprintf(stderr, "  %s!%s+0x%llX\n", module_name, symbol->Name,
                             static_cast<unsigned long long>(displacement));
            }
        } else {
            std::fprintf(stderr, "  %s+0x%llX\n", module_name,
                         static_cast<unsigned long long>(frame.AddrPC.Offset));
        }
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void install_crash_handler() {
    SetUnhandledExceptionFilter(filter);
}

}  // namespace chast_tools
