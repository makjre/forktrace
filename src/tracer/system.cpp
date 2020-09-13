/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  system
 *
 *      TODO
 */
#include "system.hpp"
#include "util.hpp"

using std::string_view;

struct SyscallInfo 
{
    unsigned argCount;
    string_view name;
};

static const SyscallInfo syscalls[] = {
    #include "syscalls.inc" // generated this bad boi with a script
};

static const string_view signals[] = {
    "None",
    "SIGHUP",
    "SIGINT",
    "SIGQUIT",
    "SIGILL",
    "SIGTRAP",
    "SIGABRT",
    "SIGBUS",
    "SIGFPE",
    "SIGKILL",
    "SIGUSR1",
    "SIGSEGV",
    "SIGUSR2",
    "SIGPIPE",
    "SIGALRM",
    "SIGTERM",
    "SIGSTKFLT",
    "SIGCHLD",
    "SIGCONT",
    "SIGSTOP",
    "SIGTSTP",
    "SIGTTIN",
    "SIGTTOU",
    "SIGURG",
    "SIGXCPU",
    "SIGXFSZ",
    "SIGVTALRM",
    "SIGPROF",
    "SIGWINCH",
    "SIGIO",
    "SIGPWR",
    "SIGSYS",
};

string_view get_signal_name(int signal) 
{
    if (0 <= signal && (size_t)signal < ARRAY_SIZE(signals)) 
    {
        return signals[signal];
    }
    return "?????";
}

string_view get_syscall_name(int syscall) 
{
    if (syscall == SYSCALL_FAKE) 
    {
        return "forktrace";
    }
    if (syscall < 0 || syscall >= (int)ARRAY_SIZE(syscalls)) 
    {
        return "?????";
    }
    return syscalls[syscall].name;
}

int get_syscall_arg_count(int syscall) 
{
    if (0 <= syscall && syscall <= (int)ARRAY_SIZE(syscalls))
    {
        return syscalls[syscall].argCount;
    }
    return -1;
}
