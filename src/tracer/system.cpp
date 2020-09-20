/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  system
 *
 *      TODO
 */
#include <fmt/core.h>

#include "system.hpp"
#include "util.hpp"
#include "ptrace.hpp"

using std::string;
using std::string_view;
using fmt::format;

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

SystemError::SystemError(int err, string_view cause) : _code(err)
{
    _msg = format("{}: {} (errno={})", cause, strerror_s(err), err);
}

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

string diagnose_wait_status(int status)
{
    if (WIFEXITED(status))
    {
        return format("exited with {}", WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        return format("killed by {} ({})", 
            get_signal_name(WTERMSIG(status)), WTERMSIG(status));
    }
    else if (WIFSTOPPED(status))
    {
        if (IS_FORK_EVENT(status))
        {
            return "fork event";
        }
        else if (IS_EXEC_EVENT(status))
        {
            return "exec event";
        }
        else if (IS_CLONE_EVENT(status))
        {
            return "clone event";
        }
        else if (IS_EXIT_EVENT(status))
        {
            return "exit event";
        }
        else if (IS_SYSCALL_EVENT(status))
        {
            return "syscall event";
        }
        else
        {
            return format("stopped by {} ({})", 
                get_signal_name(WSTOPSIG(status)), WSTOPSIG(status));
        }
    }
    return format("unknown status {}", status);
}
