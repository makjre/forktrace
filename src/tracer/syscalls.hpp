#ifndef LIBPTRACE_SYSTEM_HPP
#define LIBPTRACE_SYSTEM_HPP

/* Wouldn't be hard to port to other architectures as long as it's to Linux.
 * Main things you'd have to change would just be specific register stuff in
 * and possibly the syscall table in syscalls.inc. With a little more effort 
 * you should be able to port to other Unixes, however restructuring may be 
 * in order to make the code more architecture independent. You can forget
 * about porting this to Windows. They don't even have fork ffs. */
#ifndef __x86_64__
#error "I don't support your architecture lol."
#endif

/* This is a non-exhaustive list that just contains as many syscall numbers
 * as I need. I get all my syscall numbers from:
 *
 *  https://github.com/strace/strace/blob/master/linux/x86_64/syscallent.h
 *
 * If you want to make your own 'fake' syscalls, then just use integers less
 * than SystemCall::NONE (guaranteed to be the lowest value).
 */
enum class SystemCall : int
{
    CLONE = 56,     // Called by glibc for fork() and by pthreads.
    FORK = 57,      // Obsolete. Modern fork() wrappers call clone().
    VFORK = 58,     // punish anyone who uses this  
    EXECVE = 59,    // the only exec that is actually a syscall
    WAIT4 = 61,     // actual underlying syscall for wait & waitpid
    KILL = 62,      // sent a signal to an entire thread group
    PTRACE = 101,   // don't let people use this - might confuse us
    SETPGID = 109,  // allows a process to change its own PGID
    SETSID = 112,   // also modifies PGID (so we need to track it)
    TKILL = 200,    // send a signal to specific thread (obsolete)
    TGKILL = 234,   // send a signal to specific thread (recommended)
    WAITID = 247,   // cover all our bases
    EXECVEAT = 322, // same as execve with extra features
    NONE = -1,      // sentinel value
};

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

#endif /* LIBPTRACE_SYSTEM_HPP */
