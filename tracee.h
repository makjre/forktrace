#ifndef TRACEE_H
#define TRACEE_H

#include <unistd.h>
#include <sys/wait.h>
#include <vector>
#include <memory>
#include <string>

#include "process.h"
#include "system.h"

class Tracer; // defined in tracer.h
class Process; // defined in process.h
class Tracee; // defined in this file

/* The Tracee will raise this exception when an event appears to occur out-of-
 * order or at a strange time, or if a ptrace call fails for a strange reason.
 * If this exception is raised, the Tracee will be in a bad state, and should 
 * be either killed or detached, and the Tracee object destroyed.
 *
 * This error could happen pretty much due to three reasons:
 *
 *  (a) Someone steps in while the trace is happening and changes something
 *      or interferes with the tracer/tracee in a way that cause a ptrace call
 *      to fail or events to come in an unexpected sequence.
 *
 *  (b) Kernel bugs.
 *
 *  (c) Bugs in this program, likely due to not carefully enough implementing
 *      the ptrace semantics for some type of scenario.
 */
class BadTraceError : public std::exception {
private:
    pid_t _pid;
    std::string _message;

public:
    /* Call this with an errno value when it is a function that has failed. */
    BadTraceError(pid_t pid, int err, std::string func);

    /* Call this when a weird event occurs. */
    BadTraceError(pid_t pid, std::string message) 
        : _pid(pid), _message(message) { }

    /* Print a message describing this error to the specified output stream */
    void print(std::ostream& os) const;
    
    const char* what() const noexcept { return _message.c_str(); }
    pid_t pid() const noexcept { return _pid; } // TODO noexcept needed here?
};

/* We use this class to keep track of system calls that block. When are tracee
 * reaches a syscall-entry-stop for a blocking syscall (that we care about),
 * we'll use this class to maintain the state of the system call so that we can
 * finish it at a later time. This class may still be used to represent system
 * calls do not always block (e.g., wait/waitpid with WNOHANG). */
class BlockingCall {
public:
    virtual ~BlockingCall() { }

    /* Returns false if the tracee died while trying to prepare or finalise
     * the call. Throws an exception if some other error occurred. 
     *
     * Cleanup:
     *  If false is returned, then reaping the tracee is left to the caller.
     */
    virtual bool prepare(Tracee& t) = 0;
    virtual bool finalise(Tracee& t) = 0;
    virtual std::string_view verb() const = 0; // return a verb to describe us
};

/* A wrapper class around 'Process' that provides additional state and methods
 * for when we are actively tracing a process, rather than just representing
 * some process tree. 
 *
 * NOTE: This class needs to make sure not to consume any signals which cause
 * the tracee to stop (i.e., WIFSTOPPED==1). The Tracer needs to intercept any
 * signal that was used by the Tracer to halt the process. */
class Tracee {
private:
    enum class State {
        RUNNING,    // not stopped, halted or ended ;-)
        STOPPED,    // stopped due to an event
        HALTED,     // stopped due to user halting it (e.g., via Ctrl+C)
        ENDED,      // exited, killed or vanished (e.g. multi-threads + exec)
    };

    Tracer& _tracer; // the tracer that is managing us
    std::shared_ptr<Process> _process; // underlying process tree node
    pid_t _pid; // process ID of the tracee
    pid_t _pgid; // the process group ID of the tracee
    int _syscall; // the syscall we are currently in, otherwise: SYSCALL_NONE
    State _state;
    int _signal; // pending signal to be delivered to the tracee
    bool _execed; // have we successfully performed an exec?

    /* Keep track of if this process is currently in a blocking system call so
     * that we know how to correctly respond when the syscall exits. */
    std::unique_ptr<BlockingCall> _blockingCall;
    
    /* Private functions, comments in source file */
    void handleFork();
    void handleExec(const char* file, const char** argv);
    void handleKill(pid_t pid, int signal, bool toThread);
    void handleNewLocation(unsigned line, const char* func, const char* file);
    void handleFailedFork();
    void initiateWait(std::unique_ptr<BlockingCall> wait);
    void handleSyscallEntry(size_t args[SYS_ARG_MAX]);
    void handleSyscallExit();
    void expectSignalStop(int status);
    void expectEnded();
    bool waitForStop(int& status);
    bool resume();

    /* This pid should already have been configured to be traced correctly (by
     * that I mean PTRACE_SETOPTIONS options should have been set according to
     * the options given by Tracee::getTraceOptions). */
    Tracee(Tracer& t, pid_t pid, pid_t pgid, 
            const std::shared_ptr<Process>& parent, 
            State state) 
        : _tracer(t), _process(std::make_shared<Process>(pid, parent)), 
        _pid(pid), _pgid(pgid), _state(state), _syscall(SYSCALL_NONE), 
        _signal(0), _execed(false) { }

public:
    /* This pid should already have been configured to be traced correctly and
     * `stopped` specifies if this process is currently stopped (as in, we have
     * been notified of a stop via WIFSTOPPED==1). */
    Tracee(Tracer& t, pid_t pid, pid_t pgid,
            const std::shared_ptr<Process>& parent, 
            bool stopped = true)
        : Tracee(t, pid, pgid, parent, 
                stopped ? State::STOPPED : State::RUNNING) { }

    /* Call this when the tracee doesn't have a traced parent and we don't know
     * its current program arguments. The tracee must be configured correctly
     * and MUST currently be stopped. */
    Tracee(Tracer& t, pid_t pid, pid_t pgid) 
        : Tracee(t, pid, pgid, nullptr, State::STOPPED) { }

    /* Call this when the tracee does not have a traced parent, but we know its
     * current program arguments. The tracee must be configured correctly and
     * MUST currently be stopped. */
    Tracee(Tracer& t, pid_t pid, pid_t pgid, std::string name, 
            std::vector<std::string> args) : _tracer(t), 
        _process(std::make_shared<Process>(pid, move(name), move(args))), 
        _pid(pid), _pgid(pgid), _state(State::STOPPED), _syscall(SYSCALL_NONE), 
        _signal(0), _execed(false) { }

    /* This will make sure that the process is dead and all notifications from
     * it have been consumed. If the process is already dead, then this won't
     * do anything. */
    ~Tracee();

    Tracee(const Tracee&) = delete;
    Tracee(Tracee&&) = delete;

    /* Attempts to resume the tracee if it is currently stopped and if it is
     * not dead. Silently handles if the tracee dies. Will throw an exception 
     * if something terribly bad happens. */
    void tryResume();

    /* `status` should be the status value returned by wait/waitpid. `halting`
     * specifies whether the Tracer is currently trying to halt us. (Thus, if
     * we get a halting signal while the Tracer isn't trying to halt us, then
     * we'll just ignore it). */ 
    void handleWaitNotification(int status, bool halting = false);
    
    /* Returns a constant string that can be used to describe this process's 
     * current state (e.g., stopped, running, zombie, etc.). */
    std::string_view state() const;

    bool execed() const { return _execed; }
    bool dead() const { return _state == State::ENDED; }
    bool halted() const { return _state == State::HALTED; }
    bool stopped() const { return _state != State::RUNNING || _blockingCall; }
    Tracer& tracer() const { return _tracer; }
    const std::shared_ptr<Process>& process() const { return _process; }
    pid_t pid() const { return _pid; }
    pid_t pgid() const { return _pgid; }

    /* The ptrace options that must be used with Tracees who are managed by
     * this class. Pass these to ptrace with PTRACE_SETOPTIONS. */
    static int getTraceOptions();
};

#endif /* TRACEE_H */
