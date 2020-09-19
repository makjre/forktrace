/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  process
 *
 *      TODO
 */
#ifndef FORKTRACE_PROCESS_HPP
#define FORKTRACE_PROCESS_HPP

#include <unistd.h>
#include <vector>
#include <memory>
#include <string>
#include <optional>

#include "event.hpp"

/* This is thrown by the Process class whenever operation are done on the
 * process tree that don't make sense or aren't allowed. Why make this an
 * exception instead of using assert()? assert() shouldn't be used on stuff
 * that can depend on user input. The way that this process tree is modified
 * is according to "external input" from ptrace or whatever else. */
class ProcessTreeError : public std::exception
{
private:
    std::string _msg;
public:
    ProcessTreeError(std::string_view msg) : _msg(msg) { }
    const char* what() const noexcept { return _msg.c_str(); }
};

/* Describes a process in a process tree. This class has public functions that
 * allow users to update it with certain events as they are occurring to the
 * process. We can then later examine the history of events when drawing. */
class Process 
{
private:
    enum class State 
    {
        ALIVE,    // process is alive
        ZOMBIE,   // process is dead but hasn't been reaped yet
        REAPED,   // process is dead and was reaped by a parent
        ORPHANED, // process is dead and the reaper process had to reap it
    };

    /* History */
    pid_t _pid;
    std::weak_ptr<Process> _parent;
    std::vector<std::unique_ptr<Event>> _events;
    std::string _initialName; // process's name before any additional execs
    std::vector<std::string> _initialArgs; // ...similar thing here

    /* State */
    State _state;
    bool _killed; // have we been killed by the delivery of a signal?
    std::optional<SourceLocation> _location; // current source location

    /* Private functions, described in source file */
    void add_event(std::unique_ptr<Event> ev, bool consumeLoc = false);
    void add_event_silent(std::unique_ptr<Event> ev, bool consumeLoc = false);
    const ExecEvent* most_recent_exec(int startIndex = -1) const;

public:
    /* Call this if the process has no (traced) parent and if we don't know its
     * program arguments and name. */
    Process(pid_t pid) : _pid(pid), _state(State::ALIVE), _killed(false) { }

    /* Call this if the process doesn't have a (traced) parent, but we do know
     * its program arguments and name. */
    Process(pid_t pid, std::string_view name, std::vector<std::string> args)
        : _pid(pid), _initialName(name), _initialArgs(args), 
        _state(State::ALIVE), _killed(false) { }

    /* Call this if the process has a parent who forked/cloned us. */
    Process(pid_t pid, const std::shared_ptr<Process>& parent);

    Process(const Process&) = delete;
    Process(Process&&) = delete;

    /* These functions notify the process tree of WaitEvents and ReapEvents.
     * They can only validly be called in the following possible sequences:
     *
     *  (1) notify_waiting -> notify_failed_wait
     *  (2) notify_waiting -> notify_reaped
     *
     * In both cases, there are permitted to be event notifications delivered
     * in between the two calls (e.g., a wait call could be interrupted due to
     * the delivery of a signal). Throws ProcessTreeError if the process wasn't
     * previously notified via notify_waiting. The `waitedId` param below has 
     * the same meaning as the pid argument of waitpid(2) (incl. pids <= 0). 
     * notify_reaped will throw an error if the child isn't a zombie. */
    void notify_waiting(pid_t waitedId, bool nohang);
    void notify_failed_wait(int error); // error could be 0 for nohang
    void notify_reaped(std::shared_ptr<Process> child);

    /* Update the process tree with a fork event, with this process being the
     * parent process. */
    void notify_forked(std::shared_ptr<Process> child);

    /* Update the process tree with an exec event (success or failure). err
     * should be an errno value (e.g., 0 for success, 1 for EPERM, etc.). This
     * will try to merge consecutive failed exec events to the same path. This
     * is because the libc wrapper for exec will try different files in the
     * $PATH until it succeeds - seeing all these failures is annoying. */
    void notify_exec(std::string file, std::vector<std::string> argv, int err);

    /* Update the process tree with a death event (exited or killed). `status`
     * is the value returned by wait/waitpid. If `status` indicates that the
     * process was killed by a signal, then this function will check if the
     * most recent event was a SignalEvent with the same signal - and if so,
     * it will promote that to a killing event instead of adding a new event.*/
    void notify_ended(int status);

    /* Update the process tree with an event to indicate that this process has
     * received a signal. If the signal turns out to kill the process, then
     * notify_ended should still be called. */
    void notify_signaled(pid_t sender, int signal);

    /* Update the process tree with a signal send event (i.e., process A sends
     * a signal to process B via kill/tkill/tgkill). `killedId` should be the
     * `pid` argument of kill/tkill/tgkill. `dest` may be null if the receiving
     * process does not exist in the process tree. `toThread` specifies whether
     * a specific thread was targeted, or just the entire thread group. */
    static void notify_sent_signal(pid_t killedId, 
                                   Process& source, 
                                   Process* dest,
                                   int signal, 
                                   bool toThread = false);

    /* Update the process tree with an orphan event. Indicates that the parent
     * of this process died without reaping it (and any sub-reaper processes
     * above it also died before reaping it). Throws a ProcessTreeError if
     * called when the process isn't already a zombie. */
    void notify_orphaned();

    /* Provide this Process with a source location update. This source location
     * will be stuck onto the next eligible event that this process receives,
     * namely, fork/exec/reap events. */
    void update_location(SourceLocation location);

    /* Returns a string describing this process. */
    std::string to_string() const;

    /* Print out this process (to std::cerr) in an indented tree format. */
    void print_tree(Indent indent = 0) const;

    /* Returns a string corresponding to the name of the current _state of the
     * process (e.g., alive, zombie, etc.). Useful when logging things. */
    std::string_view state() const;

    /* This will have to first search backwards through the list of events
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
    std::string command_line(int eventIndex = -1) const;

    /* Returns a reference to the event that killed this process. An assertion
     * will fail if !dead() or if this process has no events. This reference
     * will be invalidated if non-const member functions are called. */
    const Event& death_event() const;

    bool killed() const { return _killed; }
    bool reaped() const { return _state == State::REAPED; }
    bool dead() const { return _state != State::ALIVE; }
    bool orphaned() const { return _state == State::ORPHANED; }
    pid_t pid() const { return _pid; }
    size_t event_count() const { return _events.size(); }

    /* This returns a reference that could be invalidated if any non-const
     * member functions are called - otherwise, you'll be fine. */
    const Event& event(size_t i) const { return *_events.at(i).get(); }
};

#endif /* FORKTRACE_PROCESS_HPP */
