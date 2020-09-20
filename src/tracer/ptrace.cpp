/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  ptrace
 *
 *      Functions that do all the tracing for us. I try to keep all the ptrace
 *      nastiness contained in this file.
 *
 *      The functions that manipulate memory in the tracee just use ptrace's
 *      PTRACE_PEEKDATA and PTRACE_POKEDATA options although there are nicer
 *      (more complicated) methods that would avoid as much context switching.
 */
#include <cassert> // TODO don't need
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/reg.h>
#include <sys/user.h>

#include "ptrace.hpp"
#include "system.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::runtime_error;

constexpr size_t WORD_SIZE = sizeof(size_t); // shrug

/* Taking the bitwise and of this with an address will align it to a machine
 * word boundary for this architecture. */
constexpr size_t ALIGN_MASK = WORD_SIZE - 1;

/* This will round the number up/down to the nearest multiple of the machine 
 * word size for this architecture. */
#define ALIGNED_ROUND_DOWN(x) ((x) & ~ALIGN_MASK)
#define ALIGNED_ROUND_UP(x) \
    (((x) & ALIGN_MASK) ? (ALIGNED_ROUND_DOWN(x) + WORD_SIZE) : (x)) 

/* Lmao it didn't really occur to me that C++ could do function calls at the
 * global scope (although I guess it makes sense considering that constructors
 * for globals still have to run). This is practically Python. What a joke. */
const long SYS_PAGE_SIZE = sysconf(_SC_PAGESIZE);

/* The options that we use for tracing */
constexpr int PTRACER_OPTIONS = PTRACE_O_EXITKILL
                                | PTRACE_O_TRACESYSGOOD
                                | PTRACE_O_TRACEEXEC
                                | PTRACE_O_TRACEFORK
                                | PTRACE_O_TRACECLONE;

bool get_syscall_ret(pid_t pid, size_t& retval) 
{
    errno = 0;
    unsigned long val = ptrace(PTRACE_PEEKUSER, pid, 8 * RAX, 0);
    if (errno == ESRCH) 
    {
        return false;
    }
    if (errno != 0) 
    {
        throw SystemError(errno, "ptrace(PTRACE_PEEKUSER)");
    }
    retval = val;
    return true;
}

bool set_syscall(pid_t pid, int syscall)
{
    void* addr = (void*)(8 * ORIG_RAX);
    if (ptrace(PTRACE_POKEUSER, pid, addr, (void*)(size_t)syscall) == -1) 
    {
        if (errno == ESRCH) 
        {
            return false;
        }
        throw SystemError(errno, "ptrace(PTRACE_POKEUSER)");
    }
    return true;
}

bool which_syscall(pid_t pid, int& syscall, size_t args[SYS_ARG_MAX]) 
{
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, (void*)&regs) == -1) 
    {
        if (errno == ESRCH) 
        {
            return false;
        }
        throw SystemError(errno, "ptrace(PTRACE_GETREGS)");
    }
    syscall = regs.orig_rax;
    if (args != nullptr) 
    {
        args[0] = regs.rdi;
        args[1] = regs.rsi;
        args[2] = regs.rdx;
        args[3] = regs.r10;
        args[4] = regs.r8;
        args[5] = regs.r9;
    }
    return true;
}

bool set_syscall_arg(pid_t pid, size_t val, int argIndex) 
{
    const size_t addrs[6] = {
        8 * RDI,
        8 * RSI,
        8 * RDX,
        8 * R10,
        8 * R8,
        8 * R9,
    };

    if (argIndex < 0 || argIndex >= 6) 
    {
        assert(!"Not implemented.");
    }

    void* addr = (void*)addrs[argIndex];
    if (ptrace(PTRACE_POKEUSER, pid, addr, (void*)val) == -1) 
    {
        if (errno == ESRCH) 
        {
            return false;
        }
        throw SystemError(errno, "ptrace(PTRACE_POKEUSER)");
    }
    return true;
}

bool get_tracee_result_addr(pid_t pid, void*& result) 
{
    // the stack pointer is a reasonable guess, as long as we aren't reading or
    // writing too much. To give ourselves even more margin we could round the
    // address down to the beginning of the page. I suppose if you wanted to be
    // more thorough, you could look at the memory map for the tracee or map
    // your own pages into the tracee's address space for this purpose...
    errno = 0;
    size_t addr = ptrace(PTRACE_PEEKUSER, pid, 8 * RBP, 0); 
    result = (void*)(addr & ~(SYS_PAGE_SIZE - 1));
    if (errno == ESRCH) 
    {
        return false;
    }
    if (errno != 0) 
    {
        throw SystemError(errno, "ptrace(PTRACE_PEEKUSER)");
    }
    return true;
}

/* Helper function for setup_child and start. This is how the child
 * process communicates errors back to the parent. We don't want to directly
 * return errno since it might fall inside the range for exit statuses. */
static int errno_to_exit_status(int errnoVal)
{
    switch (errnoVal)
    {
        case EBUSY:     return 1;
        case EFAULT:    return 2;
        case EINVAL:    return 3;
        case EIO:       return 4;
        case EPERM:     return 5;
        case ESRCH:     return 6;
        default:        return 7;
    }
}  

/* Reverses errno_to_exit_status. Returns 0 if no errno could be found. */
static int exit_status_to_errno(int exitStatus)
{
    switch (exitStatus)
    {
        case 1:     return EBUSY;
        case 2:     return EFAULT;
        case 3:     return EINVAL;
        case 4:     return EIO;
        case 5:     return EPERM;
        case 6:     return ESRCH;
        default:    return 0;
    }
}

/* Helper function for start() to exec the traced child process. */
static void setup_child(string_view program, vector<string> argv)
{
    // don't want children to inherit our blocked signals
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

    if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1)
    {
        _exit(errno_to_exit_status(errno));
    }

    // sync up with tracer
    raise(SIGSTOP);

    if (setpgid(0, 0) == -1)
    {
        _exit(errno_to_exit_status(errno));
    }

    // sync up with tracer
    raise(SIGSTOP);

    // Convert args to a format that exec will like
    vector<const char*> args;
    for (auto& arg : argv)
    {
        args.push_back(arg.c_str());
    }
    args.push_back(NULL);

    execvp(string(program).c_str(), (char* const*)args.data());

    // The tracer will later learn of the cause of failure via ptrace
    _exit(1); 
}

/* Kill the specified PID with SIGKILL and reap it so that it's not a zombie.
 * If the PID is not our child, then this function fails silently. If some
 * other error occurs, then a SystemError is thrown. PRESERVES ERRNO!!!!. */
static void kill_and_reap(pid_t pid) 
{
    int e = errno;

    if (kill(pid, SIGKILL) == -1 && errno != ESRCH) 
    {
        throw SystemError(errno, "kill");
    }
    
    // Just keep looping until process disappears (causing waitpid to fail).
    while (waitpid(pid, nullptr, 0) != -1) { }

    errno = e;
}

/* Helper function for start. Called when the tracee did not stop with
 * SIGSTOP as expected. We take the status that waitpid returned for the tracee
 * and throw an exception to report the tracee's error. `name` specifies the
 * name of the system call that would have failed. */
static void throw_failed_start(pid_t pid, int status, string_view name)
{
    if (WIFEXITED(status)) 
    {
        int errnoVal = exit_status_to_errno(WEXITSTATUS(status));
        throw SystemError(errnoVal, name);
    }
    if (WIFSIGNALED(status)) 
    {
        throw runtime_error("Tracee killed by unexpected signal.");
    }

    /* If we're here, the tracee hasn't died yet - so make sure of it. */
    kill_and_reap(pid);

    if (WIFSTOPPED(status)) 
    {
        throw runtime_error("Tracee stopped by unexpected signal.");
    }
    throw runtime_error("Unexpected change of state by tracee.");
}

pid_t start_tracee(string_view program, vector<string> argv)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        throw SystemError(errno, "fork");
    }
    if (pid == 0)
    {
        setup_child(program, std::move(argv));
        /* NOTREACHED */
    }

    // Ensure that ptrace(PTRACE_TRACEME, ...) succeeded, then continue the
    // tracee for the next step in the sequence.
    int status;
    if (waitpid(pid, &status, 0) == -1)
    {
        throw SystemError(errno, "waitpid");
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
    {
        throw_failed_start(pid, status, "ptrace(PTRACE_TRACEME)"); // will reap
        /* NOTREACHED */
    }
    if (ptrace(PTRACE_CONT, pid, 0, 0) == -1)
    {
        kill_and_reap(pid); // preserves errno for us
        throw SystemError(errno, "ptrace(PTRACE_CONT)");
    }

    // Ensure that the setpgid call succeeded and then configure the options
    // that we need when tracing. We'll then leave the tracee stopped for the 
    // caller to resume when they're ready.
    if (waitpid(pid, &status, 0) == -1)
    {
        throw SystemError(errno, "waitpid");
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
    {
        throw_failed_start(pid, status, "setpgid"); // reaps for us
        /* NOTREACHED */
    }
    if (ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACER_OPTIONS) == -1)
    {
        kill_and_reap(pid); // preserves errno
        throw SystemError(errno, "ptrace(PTRACE_SETOPTIONS)");
    }

    return pid;
}

bool resume_tracee(pid_t pid, int signal)
{
    // Tell ptracee to resume until it reaches a syscall-stop or other stop.
    // If we have a pending signal to deliver, we'll do that now too.
    if (ptrace(PTRACE_SYSCALL, pid, 0, signal) == -1)
    {
        if (errno == ESRCH)
        {
            return false;
        }
        throw SystemError(errno, "ptrace(PTRACE_SYSCALL)");
    }
    return true;
}

bool memset_tracee(pid_t pid, void* dest, uint8_t value, size_t len)
{
    // TODO alignment?
    int i = 0; // index of current word
    size_t* curAddr = (size_t*)dest;
    int numWords = ALIGNED_ROUND_DOWN(len) / sizeof(size_t);
    int remainder = len % sizeof(size_t);
    void* valueWord;
    memset(&valueWord, value, sizeof(valueWord));

    for (; i < numWords; ++curAddr, ++i) 
    {
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, valueWord) == -1) 
        {
            if (errno == ESRCH) 
            {
                return false;
            }
            throw SystemError(errno, "ptrace(PTRACE_POKEDATA)");
        }
    }
    if (remainder != 0) 
    {
        // Since we can only write multiples of the word size, we first need to
        // copy this word from the tracee, then change just the bytes that we
        // need, then we need to copy the word back to the tracee.
        errno = 0;
        size_t word = ptrace(PTRACE_PEEKDATA, pid, curAddr, 0);
        if (errno == ESRCH) 
        {
            return false;
        }
        if (errno != 0) 
        {
            throw SystemError(errno, "ptrace(PTRACE_PEEKDATA)");
        }

        memcpy(&word, &valueWord, remainder);

        // Now write that word back.
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, (void*)word) == -1) 
        {
            if (errno == ESRCH) 
            {
                return false;
            }
            throw SystemError(errno, "ptrace(PTRACE_POKEDATA)");
        }
    }
    return true;
}

bool copy_from_tracee(pid_t pid, void* dest, void* src, size_t len)
{
    // TODO alignment?
    int i = 0; // index of current word
    size_t word;
    size_t* curAddr = (size_t*)src;
    int numWords = ALIGNED_ROUND_UP(len) / sizeof(size_t);
    assert(numWords != 0);

    // Copy as many full words as we can
    for (;;) 
    {
        errno = 0;
        word = ptrace(PTRACE_PEEKDATA, pid, curAddr, 0);
        if (errno == ESRCH) 
        {
            return false;
        }
        if (errno != 0) 
        {
            throw SystemError(errno, "ptrace(PTRACE_PEEKDATA)");
        }
        if (i + 1 >= numWords) 
        {
            break;
        }
        ((size_t*)dest)[i] = word;
        curAddr++;
        i++;
    }

    // Copy the leftovers
    memcpy((size_t*)dest + i, &word, len % sizeof(size_t));
    return true;
}

bool copy_to_tracee(pid_t pid, void* dest, void* src, size_t len)
{
    // TODO alignment?
    int i = 0; // index of current word
    size_t* words = (size_t*)src;
    size_t* curAddr = (size_t*)dest;
    int numWords = ALIGNED_ROUND_DOWN(len) / sizeof(size_t);
    int remainder = len % sizeof(size_t);

    // Copy as many full words as we can
    for (; i < numWords; ++curAddr, ++i) 
    {
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, (void*)words[i]) == -1) 
        {
            if (errno == ESRCH) 
            {
                return false;
            }
            throw SystemError(errno, "ptrace(PTRACE_POKEDATA)");
        }
    }
    if (remainder != 0) 
    {
        // Since we can only write multiples of the word size, we first need to
        // copy this word from the tracee, then change just the bytes that we
        // need, then we need to copy the word back to the tracee.
        errno = 0;
        size_t word = ptrace(PTRACE_PEEKDATA, pid, curAddr, 0);
        if (errno == ESRCH) 
        {
            return false;
        }
        if (errno != 0) 
        {
            throw SystemError(errno, "ptrace(PTRACE_PEEKDATA)");
        }

        memcpy(&word, &words[i], remainder);

        // Now write that word back.
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, (void*)word) == -1) 
        {
            if (errno == ESRCH) 
            {
                return false;
            }
            throw SystemError(errno, "ptrace(PTRACE_POKEDATA)");
        }
    }
    return true;
}

bool copy_string_from_tracee(pid_t pid, 
                             const char* src, 
                             string& result)
{
    // The alternative methods of copying between tracer/tracee would be to
    // use process_vm_readv/process_vm_writev or /proc/$pid/mem.
    size_t *curAddr = (size_t*)src;
    result.clear();

    // TODO alignment? (does ptrace even care about it?)
    for (;;) 
    {
        errno = 0;
        size_t word = ptrace(PTRACE_PEEKDATA, pid, (void*)curAddr, 0);
        if (errno == ESRCH) 
        {
            return false;
        }
        if (errno != 0) 
        {
            throw SystemError(errno, "ptrace(PTRACE_PEEKDATA)");
        }

        char *ch = (char*)&word;
        curAddr++;
        for (size_t i = 0; i < sizeof(word); ++i) 
        {
            if (ch[i] == '\0') 
            {
                return true;
            }
            result.push_back(ch[i]);
        }
    }
}

bool copy_string_array_from_tracee(pid_t pid,
                                   const char** argv,
                                   vector<string>& args)
{
    for (;;) 
    {
        errno = 0;
        long result = ptrace(PTRACE_PEEKDATA, pid, &argv[args.size()], 0);
        if (errno == ESRCH) 
        {
            return false;
        }
        if (errno != 0) 
        {
            throw SystemError(errno, "ptrace(PTRACE_PEEKDATA)");
        }

        char* arg = (char*)result;
        if (arg == nullptr) 
        {
            return true; // we hit the NULL terminator of the array
        }

        args.emplace_back();
        if (!copy_string_from_tracee(pid, arg, args.back())) 
        {
            return false;
        }
    }
}
