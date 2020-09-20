/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  system
 *
 *      TODO
 */
#ifndef FORKTRACE_SYSTEM_HPP
#define FORKTRACE_SYSTEM_HPP

#include <string>

/* Wouldn't be hard to port to other architectures as long as it's to Linux.
 * Main things you'd have to change would just be specific register stuff in
 * and possibly the syscall table in syscalls.inc. With a little more effort 
 * you should be able to port to other Unixes, however restructuring may be 
 * in order to make the code more architecture independent. You can forget
 * about porting this to Windows. They don't even have fork ffs. */
#if !defined(__x86_64__) || !defined(__linux__)
#error "I don't support your architecture lol."
#endif

/* Throw this to describe a errno error. A bit nicer than std::system_error. */
class SystemError : public std::exception
{
private:
    std::string _msg;
    int _code;
public:
    /* The cause should be a function name or something similar. */
    SystemError(int err, std::string_view cause);
    const char* what() const noexcept { return _msg.c_str(); }
    int code() const noexcept { return _code; }
};

/* This is a non-exhaustive list that just contains as many syscall numbers
 * as I need. I get all my syscall numbers from:
 *
 *  https://github.com/strace/strace/blob/master/linux/x86_64/syscallent.h
 *
 * If you want to make your own 'fake' syscalls, then just use integers less
 * than SystemCall::NONE (guaranteed to be the lowest value).
 */
enum SystemCall : int
{
    SYSCALL_CLONE = 56,     // Called by glibc for fork() and by pthreads.
    SYSCALL_FORK = 57,      // Obsolete. Modern fork() wrappers call clone().
    SYSCALL_VFORK = 58,     // punish anyone who uses this  
    SYSCALL_EXECVE = 59,    // the only exec that is actually a syscall
    SYSCALL_WAIT4 = 61,     // actual underlying syscall for wait & waitpid
    SYSCALL_KILL = 62,      // sent a signal to an entire thread group
    SYSCALL_PTRACE = 101,   // don't let people use this - might confuse us
    SYSCALL_SETPGID = 109,  // allows a process to change its own PGID
    SYSCALL_SETSID = 112,   // also modifies PGID (so we need to track it)
    SYSCALL_TKILL = 200,    // send a signal to specific thread (obsolete)
    SYSCALL_TGKILL = 234,   // send a signal to specific thread (recommended)
    SYSCALL_WAITID = 247,   // cover all our bases
    SYSCALL_EXECVEAT = 322, // same as execve with extra features
    SYSCALL_NONE = -1,      // sentinel value
    SYSCALL_FAKE = -2,      // for our own nefarious purposes
};

/* Not Linux syscall has more than 6 arguments. */
constexpr size_t SYS_ARG_MAX = 6;

/* Some weird Linux-specific error that is returned by fork when interrupted
 * by a signal but is only visible to tracers, and is otherwise not visible
 * to userspace. */
constexpr int ERESTARTNOINTR = 513;

/* The internal error code of Linux that is only visible to tracers. This code
 * indicates that a syscall returned due to the delivery of a signal and needs
 * to be restarted. fork() won't return this (see above), but other calls that
 * block (such as wait4/waitid) will return this. */
constexpr int ERESTARTSYS = 512;

/* Get the name corresponding to a syscall number (or "?????" if none) */
std::string_view get_syscall_name(int syscall); 

/* Returns -1 if the syscall number is invalid */
int get_syscall_arg_count(int syscall);

/* Get the name corresponding to a signal number (or "?????" if none) */
std::string_view get_signal_name(int signal);

/* Returns a string describing the provided wait(2) child status. This will
 * include events like ptrace(2) events (see ptrace.hpp). If the event was
 * unknown, a string describing the raw number is returned. */
std::string diagnose_wait_status(int status);

#endif /* FORKTRACE_SYSTEM_HPP */
