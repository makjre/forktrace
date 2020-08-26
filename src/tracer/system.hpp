#ifndef SYSTEM_H
#define SYSTEM_H

#include <unistd.h>
#include <vector>
#include <string>
#include <signal.h>
#include <sys/ptrace.h>

/* See ptrace(2) man page for where these come from.
 */
#define IS_EVENT(status, event) (((status) >> 8) == (SIGTRAP | ((event) << 8)))
#define IS_FORK_EVENT(status) IS_EVENT(status, PTRACE_EVENT_FORK)
#define IS_EXEC_EVENT(status) IS_EVENT(status, PTRACE_EVENT_EXEC)
#define IS_CLONE_EVENT(status) IS_EVENT(status, PTRACE_EVENT_CLONE)
#define IS_EXIT_EVENT(status) IS_EVENT(status, PTRACE_EVENT_EXIT)
#define IS_SYSCALL_EVENT(status) (WSTOPSIG(status) == (SIGTRAP | 0x80))

/* No Linux syscall has more than six arguments.
 */
constexpr auto SYS_ARG_MAX = 6;

/* Modern libc implementations do not directly call the fork system call since
 * it is obselete. Instead, the more modern and flexible `clone` system call is
 * called instead (which is also used to create new threads). We need to figure
 * out if the clone call is equivalent to a fork.
 *
 * According to the Linux kernel source (kernel/fork.c), the `flags` argument
 * to clone (which is what we're interested in) *might* not be the first, so
 * hypothetically this *may* need to be changed when porting (probably not).
 */
#define IS_CLONE_LIKE_A_FORK(args) (((args)[0] & 0xFF) == SIGCHLD)

/* Wouldn't be hard to port to other architectures as long as it's to Linux.
 * Main things you'd have to change would just be specific register stuff in
 * system.cpp and possibly the syscall table in syscalls.h. With a little more
 * effort you should be able to port to other Unixes, however restructuring may
 * be in order to make the code more architecture independent. You can forget
 * about porting this to Windows. They don't even have fork ffs. */
#ifndef __x86_64__
#error "I don't support your architecture lol."
#endif

/* I got the syscall numbers from:
 *
 *  https://github.com/strace/strace/blob/master/linux/x86_64/syscallent.h
 */
enum SystemCall {
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
    SYSCALL_FAKE = -2,      // for our own nefarious purposes
    SYSCALL_NONE = -1,      // sentinel value
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

/* Same as strerror, except it handles the above error codes too. */
std::string_view errStr(int error);

/* Get the name corresponding to a signal number (or "?????" if none) */
std::string_view getSignalName(int signal);

/* Get the name corresponding to a syscall number (or "?????" if none) */
std::string_view getSyscallName(int syscall); 

/* An assertion fails if the syscall number isn't valid */
unsigned getSyscallArgCount(int syscall);

/* XXX: All of the below functions will return false if the underlying ptrace 
 * call fails due to ESRCH (i.e., the process no longer exists). If there is 
 * any other failure, then a system_error is thrown. For the copy/memset-type
 * functions, if a memory fault occurs (page fault in tracee's memory space,
 * then a system_error is thrown with an error code of EFAULT or EIO). 
 *
 * Cleanup:
 *  If false is returned, then 'reaping' the tracee is left to the caller. 
 */

/* Should be called after a syscall-exit-stop. Will store the return value of
 * the syscall in retval. */
bool getSyscallRet(pid_t pid, size_t& retval);

/* Should be called after a syscall-entry-stop. If `args` is NULL, then it 
 * won't write to it. */
bool whichSyscall(pid_t pid, int& syscall, size_t args[SYS_ARG_MAX]);

/* Sets the specified argument of a system call. Should only be called after a 
 * syscall-entry-stop. */
bool setSyscallArg(pid_t pid, size_t val, int argIndex);

/* Try to figure out an address in the tracee's memory space where we'll have
 * read/write permissions. We might need this if we need some 'working space'
 * in the tracee's memory to mess around. This implementation is *very* dodgy,
 * but it works! - at least for me ;-) */
bool getTraceeResultAddr(pid_t pid, void*& result);

/* Copy a block of memory from our address space to the tracee's address space.
 * If the tracee dies, false is returned. */
bool copyToTracee(pid_t pid, void* dest, void* src, size_t len);

/* Copy a block of memory from the tracee's address space to our address space.
 * If the tracee dies, false is returned. */
bool copyFromTracee(pid_t pid, void* dest, void* src, size_t len);

/* Set the bytes in a block of memory in the tracee's address space to `value`.
 * If the tracee dies, false is returned. */
bool memsetTracee(pid_t pid, void *dest, char value, size_t len);

/* Copy a null-terminated string from the specified address (`src`) in the
 * tracee's memory space and copy it over to ours (store it in `str`). */
bool copyStringFromTracee(pid_t pid, std::string& str, const char* src);

/* Copy a null-terminated array of null-terminated strings from the specified
 * address (`argv`) in the tracee's memory and store it in a vector of strings
 * on our end (`args`). */
bool copyArgsFromTracee(pid_t pid, std::vector<std::string>& args, 
        const char** argv); 

#endif /* SYSTEM_H */
