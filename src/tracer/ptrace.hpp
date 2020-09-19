/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  ptrace
 *
 *      Functions that do all the tracing for us. I try to keep all the ptrace
 *      nastiness contained in this file.
 */
#ifndef FORKTRACE_PTRACE_HPP
#define FORKTRACE_PTRACE_HPP

#include <vector>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <sys/ptrace.h>

#include "system.hpp"

/* See ptrace(2) man page for where these come from. These macros help us
 * diagnose the wait(2) statuses that we get when the tracee is stopped (so
 * you should check for WIFSTOPPED(status) first. These macros rely on the
 * ptrace options set by start_tracee.
 */
#define IS_EVENT(status, event) (((status) >> 8) == (SIGTRAP | ((event) << 8)))
#define IS_FORK_EVENT(status) IS_EVENT(status, PTRACE_EVENT_FORK)
#define IS_EXEC_EVENT(status) IS_EVENT(status, PTRACE_EVENT_EXEC)
#define IS_CLONE_EVENT(status) IS_EVENT(status, PTRACE_EVENT_CLONE)
#define IS_EXIT_EVENT(status) IS_EVENT(status, PTRACE_EVENT_EXIT)
#define IS_SYSCALL_EVENT(status) (WSTOPSIG(status) == (SIGTRAP | 0x80))

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

/* Starts a tracee using the specified program and argments. Will throw
 * system_error if a syscall failed or runtime_error if something weird 
 * happened (e.g., the tracee was killed by an unknown signal). The child 
 * is started within a new process group. Returns the pid of the tracee. 
 * The child is started in a stopped state. The tracee will be configured
 * with the following ptrace options (see man 2 ptrace for more details):
 *
 *      - PTRACE_O_EXITKILL: If we end, then the tracee gets SIGKILL'ed.
 *      - PTRACE_O_TRACEFORK: Automatically trace forked children.
 *      - PTRACE_O_TRACEEXEC: Automatically stop at the next successful exec.
 *      - PTRACE_O_TRACECLONE: Automatically trace cloned children.
 *      - PTRACE_O_TRACESYSGOOD: Helps disambiguate syscalls from other events.
 */
pid_t start_tracee(std::string_view program, std::vector<std::string> argv);

/* Resumes the traced process. Throws system_error on failure (which will
 * include if the tracee is not currently stopped). If the tracee could not
 * be found, then false is returned (i.e., ptrace gave ESRCH). If signal != 0,
 * then the specified signal will be delivered to the process when resumed. */
bool resume_tracee(pid_t pid, int signal = 0);

/* Sets a block of memory within the tracee's memory space. Will throw
 * a system_error on failure (which could be EIO if the address is bad).
 * Returns false if the tracee does not exist anymore. */
bool memset_tracee(pid_t tracee, void* dest, uint8_t value, size_t len);

/* Copies a block of memory from the tracee's address space to our address
 * space. Throws a system_error on failure (EIO if region is bad). Returns
 * false if the tracee doesn't exist anymore. */
bool copy_from_tracee(pid_t tracee, void* dest, void* src, size_t len);

/* Copies a block of memory from our address space to the tracee's address 
 * space. Throws a system_error on failure (EIO if region is bad). Returns
 * false if the tracee doesn't exist anymore. */
bool copy_to_tracee(pid_t tracee, void* dest, void* src, size_t len);

/* Copies a null-termianted string from the address space of the tracee to
 * our own address space. Throws a system_error on failure (e.g., EIO) and
 * returns false if the tracee doesn't exist anymore. */
bool copy_string_from_tracee(pid_t tracee, 
                             const char* src, 
                             std::string& result);

/* Copies a null-terminated string array (such as the argv or envp arrays).
 * from the address space of the tracee and stores the result in a vector.
 * Throws a system_error on failure (e.g., EIO if the memory region is bad)
 * and returns false if the tracee doesn't exist anymore. */
bool copy_string_array_from_tracee(pid_t tracee,
                                   const char** traceeAddr, 
                                   std::vector<std::string>& result);

/* Find somewhere in the tracee's memory space that we can use to store the
 * result of a syscall (when the tracee didn't want a result themselves).
 * Throws system_error on failure or returns false if the tracee couldn't
 * be found (e.g., ESRCH from ptrace). */
bool get_tracee_result_addr(pid_t pid, void*& result);

/* Modify the registers of the tracee to change the value of a syscall arg.
 * This should only be done when in a syscall-entry-stop. Throws system_error
 * on failure or returns false if the tracee couldn't be found. */
bool set_syscall_arg(pid_t pid, size_t val, int argIndex);

/* Modify the registers of the tracee to change the syscall number that will
 * be called. This should only be done when in a syscall-entry-stop. Throws 
 * system_error on failure or returns false if the tracee couldn't be found. */
bool set_syscall(pid_t pid, int syscall);

/* Find out the syscall number and arguments (we must be in a syscall-entry-
 * stop for this to work and make sense). Throws system_error on false or
 * returns false if the tracee couldn't be found. */
bool which_syscall(pid_t pid, int& syscall, size_t args[SYS_ARG_MAX]); 

/* Get the return value of the syscall when in a syscall-exit-stop. Throws
 * system_error on failure or returns false if the tracee doesn't exist. */
bool get_syscall_ret(pid_t pid, size_t& retval);

#endif /* FORKTRACE_PTRACE_HPP */
