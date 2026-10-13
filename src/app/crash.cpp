#include "app/crash.h"
#include "app/dbg.h"
#include "app/reslog.h"
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace crash {
namespace {

void MemoryLine(char* out, size_t n) {
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    pm.cb = sizeof(pm);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    const double mb = 1024.0 * 1024.0;
    snprintf(out, n, "working set %.0f MB, private %.0f MB, address space used %.0f / %.0f MB",
             pm.WorkingSetSize / mb, pm.PrivateUsage / mb,
             (ms.ullTotalVirtual - ms.ullAvailVirtual) / mb, ms.ullTotalVirtual / mb);
}

void Report(const char* what, EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return;
    char mem[256]; MemoryLine(mem, sizeof(mem));
    if (FILE* f = fopen("s4_crash.log", "a")) {
        const time_t t = time(nullptr);
        char when[64]; strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(f, "[%s] CRASH: %s\n", when, what);
        if (ep && ep->ExceptionRecord) {
            const EXCEPTION_RECORD* r = ep->ExceptionRecord;
            HMODULE mod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)r->ExceptionAddress, &mod);
            char modName[MAX_PATH] = "?";
            if (mod) GetModuleFileNameA(mod, modName, MAX_PATH);
            fprintf(f, "  code 0x%08lX at %p (%s +0x%lX)", r->ExceptionCode, r->ExceptionAddress, modName,
                    mod ? (unsigned long)((const char*)r->ExceptionAddress - (const char*)mod) : 0ul);
            if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
                fprintf(f, " %s 0x%p", r->ExceptionInformation[0] ? "writing" : "reading", (void*)r->ExceptionInformation[1]);
            fputc('\n', f);
        }
        fprintf(f, "  memory: %s\n", mem);
        fclose(f);
    }
    DbgLog("[crash] %s | %s", what, mem);
    HANDLE h = CreateFileA("s4_crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(h);
    }
}

// A dump of the current thread when there is no exception record (abort,
// terminate): raise one so the dump carries this thread's stack.
void ReportHere(const char* what) {
    __try { RaiseException(0xE0000001, 0, 0, nullptr); }
    __except (Report(what, GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {}
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* ep) {
    Report("unhandled SEH exception", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate() {
    std::string what = "std::terminate (uncaught C++ exception)";
    if (std::exception_ptr e = std::current_exception()) {
        try { std::rethrow_exception(e); }
        catch (const std::bad_alloc&) { what = "out of memory (std::bad_alloc)"; }
        catch (const std::exception& x) { what = std::string("uncaught exception: ") + x.what(); }
        catch (...) { what = "uncaught non-std exception"; }
    }
    ReportHere(what.c_str());
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    ReportHere("CRT invalid parameter");
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnPureCall() {
    ReportHere("pure virtual call");
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnAbort(int) {
    ReportHere("abort()");
    TerminateProcess(GetCurrentProcess(), 3);
}

}   // namespace

void Install() {
    SetUnhandledExceptionFilter(OnUnhandled);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    signal(SIGABRT, OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    LogMemory("start");
}

void LogMemory(const char* tag) {
    char mem[256]; MemoryLine(mem, sizeof(mem));
    DbgLog("[mem] %s: %s", tag, mem);
    reslog::Note("%s", tag);
}

}   // namespace crash
