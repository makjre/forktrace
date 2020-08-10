#ifndef FORKTRACE_H
#define FORKTRACE_H

#ifdef _GNU_SOURCE
#define FORKTRACE_HAS_EXECVPE
#else
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#ifdef __cplusplus
#include <cstddef>
#else
#include <stddef.h>
#endif

#define FORKTRACE_IMPLEMENT(functionName, ...)                  \
    (                                                           \
        syscall(-2, (size_t)__LINE__, __FUNCTION__, __FILE__),  \
        functionName(__VA_ARGS__)                               \
    )

#define tkill(t, s)                                             \
    (                                                           \
        syscall(-2, (size_t)__LINE__, __FUNCTION__, __FILE__),  \
        syscall(SYS_tkill, (size_t)t, (size_t)s)                \
    )

#define tgkill(p, t, s)                                         \
    (                                                           \
        syscall(-2, (size_t)__LINE__, __FUNCTION__, __FILE__),  \
        syscall(SYS_tgkill, (size_t)p, (size_t)t, (size_t)s)    \
    )

#define raise(s)                FORKTRACE_IMPLEMENT(raise, s)
#define kill(p, s)              FORKTRACE_IMPLEMENT(kill, p, s)
#define fork()                  FORKTRACE_IMPLEMENT(fork)

#define wait(s)                 FORKTRACE_IMPLEMENT(wait, s)
#define waitpid(p, s, f)        FORKTRACE_IMPLEMENT(waitpid, p, s, f)
#define waitid(t, p, i, f)      FORKTRACE_IMPLEMENT(waitid, t, p, i, f)
#define wait3(s, f, r)          FORKTRACE_IMPLEMENT(wait3, s, f, r)
#define wait4(p, s, f, r)       FORKTRACE_IMPLEMENT(wait4, p, s, f, r)

#define execv(p, v)             FORKTRACE_IMPLEMENT(execv, p, v)
#define execvp(p, v)            FORKTRACE_IMPLEMENT(execvp, p, v)
#define execve(p, v, e)         FORKTRACE_IMPLEMENT(execve, p, v, e)

#define execl(p, a0, ...) \
    FORKTRACE_IMPLEMENT(execl, p, a0 __VA_OPT__(,) __VA_ARGS__)
#define execlp(p, a0, ...) \
    FORKTRACE_IMPLEMENT(execlp, p, a0 __VA_OPT__(,) __VA_ARGS__)
#define execle(p, a0, ...) \
    FORKTRACE_IMPLEMENT(execle, p, a0 __VA_OPT__(,) __VA_ARGS__)

#ifdef FORKTRACE_HAS_EXECVPE
#define execvpe(p, v, e)        FORKTRACE_IMPLEMENT(execvpe, p, v, e)
#endif /* FORKTRACE_HAS_EXECVPE */

#endif /* FORKTRACE_H */
