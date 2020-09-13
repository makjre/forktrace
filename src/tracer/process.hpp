#ifndef PROCESS_H
#define PROCESS_H

#include <unistd.h>
#include <vector>
#include <memory>
#include <string>

#include "util.hpp"
#include "event.hpp"

class ThreadGroup; // defined in this file

/* You've all been lied to. A thread is just a process! (on Linux at least),
 * so this class describes both (POSIX) processes and threads! This class just
 * keeps track of the process tree information, whereas the Tracee class is a
 * wrapper around this that adds state and methods to manage a process that is
 * actively being traced. */
class Process {
private:
    enum class State {
        ALIVE,
        ZOMBIE,
        REAPED,
        ORPHANED,
    };

    pid_t _pid;
    std::weak_ptr<Process> _parent;
    //std::shared_ptr<ThreadGroup> _threadGroup; // null if single threaded
    std::vector<std::unique_ptr<Event>> _events;
    std::unique_ptr<SourceLocation> _location; // current source location
    State _state;
    bool _killed; // have we been killed by the delivery of a signal?

    /* These describe the name and arguments of the process before any exec
     * events occurred. */
    std::string _initialName;
    std::vector<std::string> _initialArgs;

    /* Private functions, described in source file */
    void addEvent(std::unique_ptr<Event> event, bool consumeLocation = false);
    const ExecEvent* const mostRecentExec(int startIndex = -1) const;

public:
    /* Call this if the process has no (traced) parent and if we don't know its
     * program arguments and name. */
    Process(pid_t pid) 
        : _pid(pid), _state(State::ALIVE), _killed(false) { }

    /* Call this if the process doesn't have a (traced) parent, but we do know
     * its program arguments and name. */
    Process(pid_t pid, std::string name, std::vector<std::string> args)
        : _pid(pid), _state(State::ALIVE), _killed(false), _initialName(name), 
        _initialArgs(args) { }

    /* Call this if the process has a parent who forked/cloned us. */
    Process(pid_t pid, const std::shared_ptr<Process>& parent);

    Process(const Process&) = delete;
    Process(Process&&) = delete;

    /* These functions notify the process tree of WaitEvents and ReapEvents.
     * They can only validly be called in the following possible sequences:
     *
     *  (1) notifyWaiting -> notifyFailedWait
     *  (2) notifyWaiting -> notifyReaped
     *
     * In both cases, there are permitted to be event notifications delivered
     * in between the two calls (e.g., a wait call could be interrupted due to
     * the delivery of a signal). An assertion will fail if the process wasn't
     * previously notified via notifyWaiting. The `waitedId` param below has 
     * the same meaning as the pid argument of waitpid(2) (incl. pids <= 0). */
    void notifyWaiting(pid_t waitedId, bool nohang);
    void notifyFailedWait(int error); // error could be 0 for nohang
    void notifyReaped(std::shared_ptr<Process> child);

    /* Update the process tree with a fork event, with this process being the
     * parent process. */
    void notifyForked(std::shared_ptr<Process> child);

    /* Update the process tree with an exec event (success or failure). err
     * should be an errno value (e.g., 0 for success, 1 for EPERM, etc. */
    void notifyExec(std::string file, std::vector<std::string> argv, int err);

    /* Update the process tree with a death event (exited or killed). `status`
     * is the value returned by wait/waitpid. If `status` indicates that the
     * process was killed by a signal, then this function will check if the
     * most recent event was a SignalEvent with the same signal - and if so,
     * it will remove the event and set the killing signal to be that signal.*/
    void notifyEnded(int status);

    /* Update the process tree with an event to indicate that this process has
     * received a signal. If the signal turns out to kill the process, then
     * notifyEnded should still be called. */
    void notifySignaled(pid_t sender, int signal);

    /* Update the process tree with a signal send event (i.e., process A sends
     * a signal to process B via kill/tkill/tgkill). `killedId` should be the
     * `pid` argument of kill/tkill/tgkill. `dest` may be null if the receiving
     * process does not exist in the process tree. `toThread` specifies whether
     * a specific thread was targeted, or just the entire thread group. */
    static void notifySentSignal(pid_t killedId, Process& source, 
            Process* dest, int signal, bool toThread = false);

    /* Update the process tree with an orphan event. Indicates that the parent
     * of this process died without reaping it (and any sub-reaper processes
     * above it also died before reaping it). */
    void notifyOrphaned();

    /* Provide this Process with a source location update. This source location
     * will be stuck onto the next eligible event that this process receives,
     * namely, fork/exec/reap events. */
    void updateLocation(std::unique_ptr<SourceLocation> location);

    /* Returns a string describing this process. */
    std::string toString() const;

    /* Print out this process (to the log) in an indented tree format. */
    void printTree(Indent indent = 0) const;

    /* Returns a string corresponding to the name of the current _state of the
     * process (e.g., alive, zombie, etc.). Useful when logging things. */
    std::string_view state() const;

    /* These will have to first search backwards through the list of events
     * to find if this process has done any execs. Will use this information
     * to figure out its current command line (name & args) and then return a
     * string in the format:
     *
     *  name [ args... ]
     *
     * (Brackets included.) If eventIndex is non-negative, then the search is
     * done starting from the event preceding the specified event index (an 
     * assertion will fail if the index is out-of-bounds). If the event index
     * is 0, then this means the initial command line will be returned. If the
     * index is negative, then all events will be searched.*/
    std::string commandLine(int eventIndex = -1) const;

    /* Returns a reference to the event that killed this process. An assertion
     * will fail if !dead() or if this process has no events. This reference
     * will be invalidated if non-const member functions are called. */
    const Event& deathEvent() const;

    bool killed() const { return _killed; }
    bool reaped() const { return _state == State::REAPED; }
    bool dead() const { return _state != State::ALIVE; }
    bool orphaned() const { return _state == State::ORPHANED; }
    pid_t pid() const { return _pid; }
    const Event& getEvent(size_t i) const { return *_events.at(i).get(); }
    size_t getEventCount() const { return _events.size(); }
};

/* Based on the Linux kernel notion of a thread group. A 'multi-threaded
 * process' is actually just a group of multiple processes that share most
 * of their resources (e.g., virtual memory, file table, etc...).
 */
class ThreadGroup {
private:
    pid_t _tgid; // on Linux, getpid() actually reports the TGID
    std::shared_ptr<Process> _leader; // TODO weak_ptr?
    std::vector<std::shared_ptr<Process>> _threads;

public:
    ThreadGroup(std::shared_ptr<Process> leader)
        : _tgid(leader->pid()), _leader(std::move(leader)) { }
};

/* Globally enable or disable the merging of consecutive failed execs. Ceebs
 * passing around some configuration struct to everything. */
void setExecMergingEnabled(bool enabled);

#endif /* PROCESS_H */
