#ifndef TRACER_H
#define TRACER_H

#include <memory>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <functional>
#include <cstdio>
#include <signal.h>

#include "tracee.h"

class Process; // defined in process.h

class Tracer {
private:
    enum class State {
        MARCHING,
        STEPPING,
        RUNNING,
        STOPPED,
    };

    /* The arguments given to this process. We need these so that when we fork
     * a tracee, we can correctly display its command line arguments up until
     * it execs. Not really super necessary but shrug. */
    std::vector<std::string> _tracerArgs;

    /* Keep track of the processes that are currently active. By 'active', I
     * mean the process is either currently running or is a zombie (i.e., the
     * pid is not available for recycling yet). */
    std::unordered_map<pid_t, std::unique_ptr<Tracee>> _tracees;

    /* Keep track of the PID of our direct child (which is also the PGID of the
     * tracees group). -1 indicates we have no process group yet. */
    pid_t _leader;

    /* Keep track of how many processes are currently stopped, so we know when
     * everything has stopped and the "breakpoint" has been reached. */
    unsigned _numStopped;

    /* We use this callback when we want to check if there are any newly
     * orphaned tracees. This callback should return false if there are no
     * orphans currently available. If it returns true, then it should store
     * the pid of the orphan by reference. */
    std::function<bool(pid_t&)> _getOrphanCallback;

    /* Set this to true if we're currently trying to halt everyone. We use this
     * flag to avoid sending multiple halt signals before we are done with the
     * halting and so that we can ignore halt signals that arrive late. We also
     * use this so others can know if we stopped due to a halt signal. */
    std::atomic<bool> _haltSent;

    /* Keep track of what we're currently trying to do. This isn't critical,
     * we just use it to avoid sending halt signals when we don't need to. */
    std::atomic<State> _state;
    std::atomic<pid_t> _continuedPID; // pid if _state==State::STEPPING
    
    /* Stores PIDs that have been recycled by the system. This can occur when
     * the reaper process reaps a tracee, but then the system recycles its PID
     * before we get notified about it. Each time we encounter a recycled PID,
     * we pop it onto the end of this vector. When collecting PIDs of orphans,
     * we then check this vector first, to make sure we don't get confused into
     * thinking that a currently running process has been orphaned. */
    std::vector<pid_t> _recycledPIDs;

    /* Private functions, see source file for comments */
    void collectOrphans();
    bool removeIfDeadChild(decltype(_tracees)::iterator& it);
    auto removeTracee(decltype(_tracees)::iterator it); // returns iterator ;-)
    bool resumeTracee(decltype(_tracees)::iterator it);
    void resumeAllTracees();
    void handleWaitNotification(decltype(_tracees)::iterator it, int status);
    void updateStopCount(decltype(_tracees)::iterator it, bool stoppedBefore,
            bool stoppedAfter);
    void signalAll(int signal);

public:
    /* This constructor asks for the arguments provided to the program. This
     * is so that when we create a new child process, we are able to show its
     * arguments before it does the initial exec. */
    Tracer(std::vector<std::string> tracerArgs, 
            std::function<bool(pid_t&)> func) 
        : _tracerArgs(tracerArgs), _leader(-1), _numStopped(0), 
        _getOrphanCallback(func), _haltSent(false), _state(State::STOPPED) { }

    Tracer(const Tracer&) = delete;
    Tracer(Tracer&&) = delete;

    /**************************************************************************
     * FOR TRACEES
     *************************************************************************/

    /* Add a process to the list of active processes. This causes an exception
     * to be thrown if the process is already in the list. NOT THREAD SAFE. */
    void addTracee(std::unique_ptr<Tracee> t);

    /* Remove a process from the list of active processes. Call this when a
     * process's PID is available for re-use. Throws an exception if PID not in
     * list. ***A Tracee should never remove itself***. NOT THREAD SAFE. (Note:
     * will update _numStopped). */
    void removeTracee(pid_t pid);

    /* Return a constant reference to a tracee from our list of active PIDs. 
     * If it's not in the list, then an exception is thrown. The reference will
     * at live at least as long as the tracee remains in our list of active
     * tracees (until removeTracee is called). This is NOT THREAD SAFE. */
    const Tracee& operator[](pid_t pid);

    /* Returns a shared_ptr to the process tree node for the specified PID, or
     * null if no active tracee could be found with the PID. NOT THREAD SAFE.*/
    std::shared_ptr<Process> findNode(pid_t pid);

    int getHaltSignal() const { return SIGTRAP; } // signal used to halt

    /**************************************************************************
     * FOR USERS
     *************************************************************************/

    /* Start a tracee from command line arguments. This will be done via the
     * exec*p family of exec functions, so the path will be searched. This
     * tracee will become our child and the leader process. This will throw an 
     * exception on failure (which includes if any tracees are still alive from
     * a previous session). This returns a shared pointer to the process tree 
     * object that represents the created process. Here, `childArgs` is a NULL-
     * terminated array. Each version must be given have at least one argument 
     * (i.e., argv[0]). NOT THREAD SAFE. */
    std::shared_ptr<Process> start(const char** childArgs);
    std::shared_ptr<Process> start(const std::vector<std::string>& childArgs);

    /* Continuously resumes all tracees until they are all dead. If a tracee
     * is found to have been halted during this call, then the call waits for
     * all tracees to be halted and then stops and returns. */
    void go();

    /* Continue all tracees until they all stop. Returns true if there are any
     * tracees remaining and false if all are dead. NOT THREAD SAFE. */
    bool march();

    /* Continues a single tracee with the specified PID until it stops. Throws
     * a runtime_error if the provided PID is not valid. NOT THREAD SAFE. */ 
    void step(pid_t pid);

    /* SIGKILLs all of the tracees and then reaps them all, then collects all
     * orphans and clears the list of tracees. Does not log the state of any
     * of the killed tracees out in the process. NOT THREAD SAFE. */
    void nuke();

    /* If all tracees are being continued, then halt all tracees where they 
     * stand. If only one tracee is being continued, then just halt that one.
     * Otherwise, do nothing. XXX: This method is the only thread-safe public 
     * member function for this class. (It is intended to be used so that a 
     * sigwait-ing thread can be used to halt all the tracees upon receiving 
     * SIGINT). */
    void halt();

    /* Send SIGKILL to all tracees. NOT THREAD SAFE. */
    void killAll();

    /* Prints a list of all the active processes to cout. NOT THREAD SAFE */
    void printList() const;

    bool halted() const { return _haltSent; }
    bool traceesAlive() const { return !_tracees.empty(); }
    size_t traceeCount() const { return _tracees.size(); }
};

#endif /* TRACER_H */
