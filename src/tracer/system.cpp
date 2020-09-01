/* I use ptrace to read/write from the tracee's memory space here. You can
 * alternatively use process_vm_readv/process_vm_writev or even procfs to do
 * this (and doing so may be more efficient to, since there would be less
 * context switches), but using ptrace is just simpler and is good enough. */

#include <system_error>
#include <cerrno>
#include <cassert>
#include <cstring>
#include <sys/ptrace.h>
#include <sys/reg.h>
#include <sys/user.h>

#include "system.hpp"
#include "util.hpp"

using namespace std;

constexpr size_t WORD_SIZE = sizeof(size_t); // shrug

/* Taking the bitwise and of this with an address will align it to a machine
 * word boundary for this architecture.
 */
constexpr size_t ALIGN_MASK = WORD_SIZE - 1;

/* This will round the number up/down to the nearest multiple of the machine 
 * word size for this architecture.
 */
#define ALIGNED_ROUND_DOWN(x) ((x) & ~ALIGN_MASK)
#define ALIGNED_ROUND_UP(x) \
    (((x) & ALIGN_MASK) ? (ALIGNED_ROUND_DOWN(x) + WORD_SIZE) : (x)) 

/* Lmao it didn't really occur to me that C++ could do function calls at the
 * global scope (although I guess it makes sense considering that constructors
 * for globals still have to run). This is practically Python. What a joke. */
const long SYS_PAGE_SIZE = sysconf(_SC_PAGESIZE);

struct SyscallInfo {
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

string_view errStr(int error) {
    if (error == ERESTARTNOINTR) {
        return "ERESTARTNOINTR";
    }
    if (error == ERESTARTSYS) {
        return "ERESTARTSYS";
    }
    return strerror(error);
}

string_view getSignalName(int signal) {
    if (0 <= signal && signal < ARRAY_SIZE(signals)) {
        return signals[signal];
    }
    return "?????";
}

string_view getSyscallName(int syscall) {
    if (syscall == SYSCALL_FAKE) {
        return "forktrace";
    }
    if (syscall < 0 || syscall >= ARRAY_SIZE(syscalls)) {
        return "?????";
    }
    return syscalls[syscall].name;
}

unsigned getSyscallArgCount(int syscall) {
    assert(0 <= syscall && syscall <= ARRAY_SIZE(syscalls));
    return syscalls[syscall].argCount;
}

bool getSyscallRet(pid_t pid, size_t& retval) {
    errno = 0;
    unsigned long val = ptrace(PTRACE_PEEKUSER, pid, 8 * RAX, 0);
    if (errno == ESRCH) {
        return false;
    }
    if (errno != 0) {
        throw system_error(errno, generic_category(), "ptrace");
    }

    retval = val;
    return true;
}

bool whichSyscall(pid_t pid, int& syscall, size_t args[SYS_ARG_MAX]) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, (void *)&regs) == -1) {
        if (errno == ESRCH) {
            return false;
        }
        throw system_error(errno, generic_category(), "ptrace");
    }

    syscall = regs.orig_rax;
    if (args != nullptr) {
        args[0] = regs.rdi;
        args[1] = regs.rsi;
        args[2] = regs.rdx;
        args[3] = regs.r10;
        args[4] = regs.r8;
        args[5] = regs.r9;
    }
    return true;
}

bool setSyscallArg(pid_t pid, size_t val, int argIndex) {
    const size_t addrs[6] = {
        8 * RDI,
        8 * RSI,
        8 * RDX,
        8 * R10,
        8 * R8,
        8 * R9,
    };

    if (argIndex < 0 || argIndex >= 6) {
        assert(!"Not implemented.");
    }

    if (ptrace(PTRACE_POKEUSER, pid, (void *)addrs[argIndex], (void *)val) 
            == -1) {
        if (errno == ESRCH) {
            return false;
        }
        throw system_error(errno, generic_category(), "ptrace");
    }
    return true;
}

bool getTraceeResultAddr(pid_t pid, void*& result) {
    // the stack pointer is a reasonable guess, as long as we aren't reading or
    // writing too much. To give ourselves even more margin we could round the
    // address down to the beginning of the page. I suppose if you wanted to be
    // more thorough, you could look at the memory map for the tracee...
    errno = 0;
    size_t addr = ptrace(PTRACE_PEEKUSER, pid, 8 * RBP, 0); 
    result = (void*)(addr & ~(SYS_PAGE_SIZE - 1));
    if (errno == ESRCH) {
        return false;
    }
    if (errno != 0) {
        throw system_error(errno, generic_category(), "ptrace");
    }
    return true;
}

bool copyToTracee(pid_t pid, void* dest, void* src, size_t len) {
    // TODO alignment?
    int i = 0; // index of current word
    size_t* words = (size_t*)src;
    size_t* curAddr = (size_t*)dest;
    int numWords = ALIGNED_ROUND_DOWN(len) / sizeof(size_t);
    int remainder = len % sizeof(size_t);

    for (; i < numWords; ++curAddr, ++i) {
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, (void*)words[i]) == -1) {
            if (errno == ESRCH) {
                return false;
            }
            throw system_error(errno, generic_category(), "ptrace");
        }
    }

    if (remainder != 0) {
        // Since we can only write multiples of the word size, we first need to
        // copy this word from the tracee, then change just the bytes that we
        // need, then we need to copy the word back to the tracee.
        errno = 0;
        size_t word = ptrace(PTRACE_PEEKDATA, pid, curAddr, 0);
        if (errno == ESRCH) {
            return false;
        }
        if (errno != 0) {
            throw system_error(errno, generic_category(), "ptrace");
        }

        memcpy(&word, &words[i], remainder);

        // Now write that word back.
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, (void*)word) == -1) {
            if (errno == ESRCH) {
                return false;
            }
            throw system_error(errno, generic_category(), "ptrace");
        }
    }

    return true;
}

bool copyFromTracee(pid_t pid, void* dest, void* src, size_t len) {
    // TODO alignment?
    int i = 0; // index of current word
    size_t word;
    size_t* curAddr = (size_t*)src;
    int numWords = ALIGNED_ROUND_UP(len) / sizeof(size_t);
    assert(numWords != 0);

    for (;;) {
        errno = 0;
        word = ptrace(PTRACE_PEEKDATA, pid, curAddr, 0);
        if (errno == ESRCH) {
            return false;
        }
        if (errno != 0) {
            throw system_error(errno, generic_category(), "ptrace");
        }

        if (i + 1 >= numWords) {
            break;
        }

        ((size_t*)dest)[i] = word;
        curAddr++;
        i++;
    }

    memcpy((size_t*)dest + i, &word, len % sizeof(size_t));
    return true;
}

bool memsetTracee(pid_t pid, void *dest, char value, size_t len) {
    // TODO alignment?
    int i = 0; // index of current word
    size_t* curAddr = (size_t*)dest;
    int numWords = ALIGNED_ROUND_DOWN(len) / sizeof(size_t);
    int remainder = len % sizeof(size_t);
    void* valueWord;
    memset(&valueWord, value, sizeof(valueWord));

    for (; i < numWords; ++curAddr, ++i) {
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, valueWord) == -1) {
            if (errno == ESRCH) {
                return false;
            }
            throw system_error(errno, generic_category(), "ptrace");
        }
    }

    if (remainder != 0) {
        // Since we can only write multiples of the word size, we first need to
        // copy this word from the tracee, then change just the bytes that we
        // need, then we need to copy the word back to the tracee.
        errno = 0;
        size_t word = ptrace(PTRACE_PEEKDATA, pid, curAddr, 0);
        if (errno == ESRCH) {
            return false;
        }
        if (errno != 0) {
            throw system_error(errno, generic_category(), "ptrace");
        }

        memcpy(&word, &valueWord, remainder);

        // Now write that word back.
        if (ptrace(PTRACE_POKEDATA, pid, curAddr, (void*)word) == -1) {
            if (errno == ESRCH) {
                return false;
            }
            throw system_error(errno, generic_category(), "ptrace");
        }
    }

    return true;
}

bool copyStringFromTracee(pid_t pid, string& str, const char* src) {
    /* The alternative methods of copying between tracer/tracee would be to
     * use process_vm_readv/process_vm_writev or /proc/$pid/mem. */
    size_t *curAddr = (size_t *)src;
    str.clear();

    //TODO alignment? (does ptrace even care about it?)
    for (;;) {
        errno = 0;
        size_t word = ptrace(PTRACE_PEEKDATA, pid, (void *)curAddr, 0);
        if (errno == ESRCH) {
            return false;
        }
        if (errno != 0) {
            throw system_error(errno, generic_category(), "ptrace");
        }

        char *ch = (char *)&word;
        curAddr++;
        for (int i = 0; i < sizeof(word); ++i) {
            if (ch[i] == '\0') {
                return true;
            }
            str.push_back(ch[i]);
        }
    }
}

bool copyArgsFromTracee(pid_t pid, vector<string>& args, const char** argv) {
    for (;;) {
        errno = 0;
        long result = ptrace(PTRACE_PEEKDATA, pid, &argv[args.size()], 0);
        if (errno == ESRCH) {
            return false;
        }
        if (errno != 0) {
            throw system_error(errno, generic_category(), "errno");
        }

        char* arg = (char *)result;
        if (arg == nullptr) {
            return true;
        }

        args.emplace_back();
        if (!copyStringFromTracee(pid, args.back(), arg)) {
            return false;
        }
    }
}

