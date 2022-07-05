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
#include <mutex>
#include <queue>
#include <functional>

class Process; // defined in process.hpp
struct Tracee;
class Tracer;
class BlockingCall; // defined in tracer.cpp

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

/* Used for book-keeping by the Tracer class. */
struct Tracee
{
    enum State
    {
        RUNNING,
        STOPPED,
        DEAD,
    };

    pid_t pid;
    State state;
    int syscall;    // Current syscall, SYSCALL_NONE if not in one
    int signal;     // Pending signal to be delivered when next resumed
    std::unique_ptr<BlockingCall> blockingCall;
    std::shared_ptr<Process> process;

    /* Create a tracee started in the stopped state */
    Tracee(pid_t pid, std::shared_ptr<Process> process);

    /* Move constructor needed in some cases (or else STL gibberish ensues). */
    Tracee(Tracee&&);

    /* Need a destructor in source file to keep std::unique_ptr happy... */
    ~Tracee();
};

/* All the public member functions are "thread-safe". */
class Tracer 
{
private:
    /* This class is defined in tracer.cpp and needs access to us to help
     * handle a successful wait call. I could make public member functions
     * for that but I don't want to expose those functions to everyone. */
    template<class, bool, int> friend class WaitCall;

    /* We use a single lock for everything to keep it all simple. Currently,
     * only the public functions lock it - private functions are all unlocked
     * and rely on the public functions to do the locking for them. This also
     * means that you'll need to think twice before calling a public function
     * from a private function (since you could get a deadlock). */
    mutable std::mutex _lock;

    /* Keep track of the processes that are currently active. By 'active', I
     * mean the process is either currently running or is a zombie (i.e., the
     * pid is not available for recycling yet). */
    std::unordered_map<pid_t, Tracee> _tracees;

    /* A queue of all the orphans that we've been notified about. We don't
     * handle them straight away since notify_orphan may be called from a
     * separate thread and we want to be able to print error messages and
     * throw exceptions in the main thread that calls step(). */
    std::queue<pid_t> _orphans;
    
    struct Leader
    {
        bool execed; // has the initial exec succeeded yet?
        Leader() : execed(false) { }
    };

    /* Keep track of the PIDs of our direct children. */
    std::unordered_map<pid_t, Leader> _leaders;
    
    /* Stores PIDs that have been recycled by the system. This can occur when
     * the reaper process reaps a tracee, but then the system recycles its PID
     * before we get notified about it. Each time we encounter a recycled PID,
     * we pop it onto the end of this vector. When collecting PIDs of orphans,
     * we then check this vector first, to make sure we don't get confused into
     * thinking that a currently running process has been orphaned. */
    std::vector<pid_t> _recycledPIDs;

    /* Private functions, see source file */
    void _collect_orphans();
    bool _are_tracees_running() const;
    bool _all_tracees_dead() const;
    bool _resume(Tracee&);
    bool _wait_for_stop(Tracee&, int&);
    void _handle_wait_notification(pid_t, int);
    void _handle_wait_notification(Tracee&, int);
    void _handle_syscall_entry(Tracee&, int, size_t[]);
    void _handle_syscall_exit(Tracee&);
    void _handle_fork(Tracee&);
    void _handle_failed_fork(Tracee&);
    void _handle_exec(Tracee&, const char*, const char**);
    void _handle_kill(Tracee&, pid_t, int, bool);
    void _handle_new_location(Tracee&, unsigned, const char*, const char*);
    void _handle_signal_stop(Tracee&, int);
    void _handle_stopped(Tracee&, int);
    Tracee& _add_tracee(pid_t, std::shared_ptr<Process>);
    void _expect_ended(Tracee&);
    void _initiate_wait(Tracee&, std::unique_ptr<BlockingCall>);
    void _on_sent_signal(Tracee&, pid_t, int, bool);

public:
    Tracer() { }

    Tracer(const Tracer&) = delete;
    Tracer(Tracer&&) = delete;
    ~Tracer() { }

    /* Start a tracee from command line arguments. The path will be searched
     * for the program. This tracee will become our child and the new leader 
     * process. The args list includes argv[0]. Throws either a SystemError
     * or runtime_error on failure. */
    std::shared_ptr<Process> start(std::string_view path, 
                                   std::vector<std::string> argv);

    /* Continue all tracees until they all stop. Returns true if there are any
     * tracees remaining (whether they are alive or dead) - e.g., if there are
     * orphaned tracees that we haven't been notified about via notify_orphan
     * yet, then this will still return true (even if all are dead). */
    bool step();

    /* Notify the tracer that an orphan has been reaped by the reaper process.
     * This function is safe to call from a separate thread. */
    void notify_orphan(pid_t pid);

    /* Will ask the tracer to check if it has recently been notified of any
     * orphans and if it has, to handle those now (instead of later). We use
     * this to implement a bash-like feature where pressing enter will cause
     * bash to show any jobs that were killed since the last command. Whether
     * this ends up doing anything depends on the timings etc. since step()
     * will do these checks itself (so not calling this isn't a big deal). */
    void check_orphans();

    /* Will forcibly kill everything. Safe to call from separate thread. */
    void nuke();

    /* Prints a list of all the active processes to std::cerr. */
    void print_list() const;

    /* Return true if any tracees are still alive (zombies aren't counted). */
    bool tracees_alive() const;

    /* Return true if any tracees still exist (zombies are counted). */
    bool tracees_exist() const { return !_tracees.empty(); }
};

#endif /* FORKTRACE_TRACER_HPP */
