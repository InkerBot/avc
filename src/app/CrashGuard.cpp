#include "app/CrashGuard.hpp"

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <iterator>
#include <string>

namespace avc::app {
namespace {

wchar_t g_crash_file[32768]{};

LONG WINAPI onFatalException(EXCEPTION_POINTERS *exception)
{
    if (exception == nullptr || exception->ExceptionRecord == nullptr || g_crash_file[0] == L'\0') {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(exception->ExceptionRecord->ExceptionAddress);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           address, &module)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    wchar_t module_path[32768]{};
    const DWORD length = GetModuleFileNameW(module, module_path, 32768);
    if (length == 0 || length == 32768) return EXCEPTION_CONTINUE_SEARCH;

    const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, module_path, static_cast<int>(length),
                                               nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(std::max(utf8_size, 0)), '\0');
    if (utf8_size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, module_path, static_cast<int>(length), utf8.data(),
                            utf8_size, nullptr, nullptr);
    }
    const HANDLE file = CreateFileW(g_crash_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        WriteFile(file, "\n", 1, &written, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}

void installCrashGuard(const std::filesystem::path &crash_file)
{
    const std::wstring path = crash_file.wstring();
    if (path.size() >= std::size(g_crash_file)) return;
    std::copy(path.begin(), path.end(), g_crash_file);
    g_crash_file[path.size()] = L'\0';
    SetUnhandledExceptionFilter(&onFatalException);
}

}

#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstring>

namespace avc::app {
namespace {

char g_crash_file[4096];
stack_t g_alt_stack{};
char *g_alt_storage = nullptr;

void *faultingPc(void *context)
{
    if (context == nullptr) {
        return nullptr;
    }
    auto *uc = static_cast<ucontext_t *>(context);
#if defined(__x86_64__)
    return reinterpret_cast<void *>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
    return reinterpret_cast<void *>(uc->uc_mcontext.gregs[REG_EIP]);
#elif defined(__aarch64__)
    return reinterpret_cast<void *>(uc->uc_mcontext.pc);
#else
    (void)uc;
    return nullptr;
#endif
}

extern "C" void onFatalSignal(int signum, siginfo_t * , void *context)
{
    void *pc = faultingPc(context);
    Dl_info where{};
    if (pc != nullptr && g_crash_file[0] != '\0' && ::dladdr(pc, &where) != 0
        && where.dli_fname != nullptr) {
        const int fd = ::open(g_crash_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            const std::size_t length = ::strlen(where.dli_fname);
            ssize_t written = ::write(fd, where.dli_fname, length);
            written = ::write(fd, "\n", 1);
            (void)written;
            ::close(fd);
        }
    }

    // Back to the default and round again, so that the process dies of what
    // actually killed it and the daemon sees the real signal.
    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    ::sigemptyset(&restore.sa_mask);
    ::sigaction(signum, &restore, nullptr);
    ::raise(signum);
}

}

void installCrashGuard(const std::filesystem::path &crash_file)
{
    const std::string path = crash_file.string();
    if (path.size() >= sizeof(g_crash_file)) {
        return;
    }
    std::memcpy(g_crash_file, path.c_str(), path.size() + 1);

    // Its own stack, because a plugin that runs off the end of the audio
    // thread's stack leaves no room to handle the signal that says so.
    if (g_alt_storage == nullptr) {
        g_alt_storage = new char[SIGSTKSZ];
        g_alt_stack.ss_sp = g_alt_storage;
        g_alt_stack.ss_size = SIGSTKSZ;
        g_alt_stack.ss_flags = 0;
        ::sigaltstack(&g_alt_stack, nullptr);
    }

    struct sigaction action{};
    action.sa_sigaction = &onFatalSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    ::sigemptyset(&action.sa_mask);

    for (const int signum : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        ::sigaction(signum, &action, nullptr);
    }
}

}
#endif
