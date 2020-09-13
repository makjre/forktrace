/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  tracer
 *
 *      TODO
 */
#ifndef FORKTRACE_TRACER_HPP
#define FORKTRACE_TRACER_HPP

#include <memory>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <functional>
#include <cstdio>
#include <signal.h>

#include "system.hpp"

class Process; // defined in process.hpp
struct Tracee;
class Tracer;

/* The tracer will raise this exception when an event appears to occur out-of-
 * order or at a strange time. If this exception is raised, the tracer will
 * stop tracking the pid and will leave it as is - (it should be either killed 
 * or detached). This error could happen pretty much due to three reasons:
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
class BadTraceError : public std::exception 
{
private:
    pid_t _pid;
    std::string _message;

public:
    /* Call this when a weird event occurs. */
    BadTraceError(pid_t pid, std::string_view message);
    
    const char* what() const noexcept { return _message.c_str(); }
    pid_t pid() const noexcept { return _pid; } // TODO noexcept needed here?
};

/* We use this class to keep track of blocking system calls. When a tracee 
 * reaches a syscall-entry-stop for a blocking syscall that we care about,
 * we'll use this class to maintain the state of the system call so that we
 * can finish it at a later time. This class may still be used to represent
 * system calls do not always block (e.g., wait/waitpid with WNOHANG). */
class BlockingCall 
{
public:
    virtual ~BlockingCall() { }

    /* Returns false if the tracee died while trying to prepare or finalise
     * the call. Throws an exception if some other error occurred. 
     *
     * Cleanup:
     *  If false is returned, then reaping the tracee is left to the caller
     */
    virtual bool prepare(Tracer& tracer, Tracee& tracee) = 0;
    virtual bool finalise(Tracer& tracer, Tracee& tracee) = 0;
};

/* Used for book-keeping by the Tracer class. */
struct Tracee
{
    pid_t pid;
    bool stopped;
    int syscall;    // Current syscall, SYSCALL_NONE if not in one
    int signal;     // Pending signal to be delivered when next resumed
    std::shared_ptr<Process> process;
    std::unique_ptr<BlockingCall> blockingCall;

    Tracee(pid_t pid, std::shared_ptr<Process> process)
        : pid(pid), stopped(true), syscall(SYSCALL_NONE),
        signal(0), process(std::move(process)) { }

    /* Need a default constructor for use with unordered_map<>::operator[] */
    Tracee() : pid(-1), stopped(false), syscall(SYSCALL_NONE), signal(0) { }

    /* See tracer.cpp */
    static void remove_tracee(Tracer& tracer, pid_t pid);
};

class Tracer 
{
private:
    friend struct Tracee; // needed for remove_tracee in tracer.cpp

    /* Keep track of the processes that are currently active. By 'active', I
     * mean the process is either currently running or is a zombie (i.e., the
     * pid is not available for recycling yet). */
    std::unordered_map<pid_t, Tracee> _tracees;

    /* Keep track of the PID of our direct child (which is also the PGID of the
     * tracee's group). -1 indicates we have no process group yet. */
    pid_t _leader;

    /* Keep track of whether the leader has successfully execed yet or not. */
    bool _execed;

    /* We use this callback when we want to check if there are any newly
     * orphaned tracees. This callback should return false if there are no
     * orphans currently available. If it returns true, then it should store
     * the pid of the orphan by reference. */
    std::function<bool(pid_t&)> _getOrphanCallback;
    
    /* Stores PIDs that have been recycled by the system. This can occur when
     * the reaper process reaps a tracee, but then the system recycles its PID
     * before we get notified about it. Each time we encounter a recycled PID,
     * we pop it onto the end of this vector. When collecting PIDs of orphans,
     * we then check this vector first, to make sure we don't get confused into
     * thinking that a currently running process has been orphaned. */
    std::vector<pid_t> _recycledPIDs;

    /* Private functions, see source file */
    bool all_tracees_stopped() const;
    void collect_orphans();
    bool resume(Tracee&);
    bool wait_for_stop(Tracee&, int&);
    void handle_wait_notification(pid_t, int);
    void handle_wait_notification(Tracee&, int);
    void handle_syscall_entry(Tracee&, int, size_t[]);
    void handle_syscall_exit(Tracee&);
    void handle_fork(Tracee&);
    void handle_failed_fork(Tracee&);
    void handle_exec(Tracee&, const char*, const char**);
    void handle_new_location(Tracee&, unsigned, const char*, const char*);
    void handle_signal_stop(Tracee&, int);
    Tracee& add_tracee(pid_t, std::shared_ptr<Process>);
    void expect_ended(Tracee&);
    void initiate_wait(Tracee&, std::unique_ptr<BlockingCall>);

public:
    Tracer(std::function<bool(pid_t&)> func) 
        : _leader(-1), _execed(false), _getOrphanCallback(func) { }

    Tracer(const Tracer&) = delete;
    Tracer(Tracer&&) = delete;
    ~Tracer() { }

    /* Start a tracee from command line arguments. The path will be searched
     * for the program. This tracee will become our child and the new leader 
     * process. This will fail and return null if tracees are still alive from
     * a previous session. The args list includes argv[0]. Throws system_error
     * or runtime_error on failure. */
    std::shared_ptr<Process> start(std::string_view path, 
                                   std::vector<std::string> argv);

    /* Finds a process with the specified PID or returns null if none. */
    std::shared_ptr<Process> find(pid_t pid);

    /* Continue all tracees until they all stop. Returns true if there are any
     * tracees remaining and false if all are dead. */
    bool step();

    /* Will forcibly kill everything. This is the only thread-safe function. */
    void nuke();

    /* Prints a list of all the active processes to std::cerr. */
    void print_list() const;

    bool tracees_alive() const { return !_tracees.empty(); }
};

#endif /* FORKTRACE_TRACER_HPP */
