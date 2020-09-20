#include <cassert>
#include <iostream>
#include <fmt/core.h>
#include <unistd.h>

#include "process.hpp"
#include "log.hpp"
#include "util.hpp"
#include "system.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::unique_ptr;
using std::shared_ptr;
using std::make_unique;
using std::make_shared;
using fmt::format;

/* A helper function that throws a ProcessTreeError with a message formatted
 * using format if a certain condition wasn't met. */
template<typename ...Args>
void process_assert(bool cond, std::string_view fmt, Args... args)
{
    if (!cond)
    {
        throw ProcessTreeError(format(fmt, args...));
    }
}

Process::Process(pid_t pid, const shared_ptr<Process>& parent)
    : _pid(pid), _parent(parent), _state(State::ALIVE), _killed(false)
{
    const ExecEvent* lastExec = parent->most_recent_exec();
    if (!lastExec) 
    {
        _initialName = parent->_initialName;
        _initialArgs = parent->_initialArgs;
    } 
    else 
    {
        _initialName = lastExec->file();
        _initialArgs = lastExec->args;
    }
}

/* Will do a reverse search to find the most recent successful exec event for 
 * this process, and will return null if it couldn't be found. The pointer will
 * become invalid if the event is removed from our list. If startIndex is
 * negative, then we'll search all events. Otherwise, we'll start the reverse
 * search from the event preceding the specified index. */
const ExecEvent* Process::most_recent_exec(int startIndex) const 
{
    if (_events.empty() || startIndex == 0) 
    {
        return nullptr;
    }
    if (startIndex < 0) 
    {
        startIndex = _events.size();
    }
    startIndex--;
    for (long i = startIndex; i >= 0; --i) 
    {
        if (auto exec = dynamic_cast<const ExecEvent*>(_events.at(i).get()))
        {
            if (exec->succeeded()) 
            {
                return exec;
            }
        }
    }
    return nullptr;
}

/* Add this event to the list and log it out. Throws ProcessTreeError if the
 * process has already ended. If `consumeLocation` is true, then the current
 * source location is **moved** into the provided event if it exists. Events
 * can only be added if the process is alive. */
void Process::add_event(unique_ptr<Event> event, bool consumeLocation) 
{
    process_assert(_state == State::ALIVE,
        "add_event({}) called when state != ALIVE", event->to_string());
    if (_location.has_value() && consumeLocation)
    {
        log("{} @ {}", event->to_string(), _location->to_string());
        event->location = std::move(_location);
        _location.reset();
    }
    else
    {
        log("{}", event->to_string());
    }
    _events.push_back(std::move(event));
}

void Process::notify_waiting(pid_t waitedId, bool nohang) 
{
    // If the very last event was a failed wait event with ERESTARTSYS, then
    // we'll just merge the two together (we only really care about showing
    // them separately when another event appears in between them).
    if (!_events.empty()) 
    {
        if (auto wait = dynamic_cast<WaitEvent*>(_events.back().get())) 
        {
            if (wait->error == ERESTARTSYS) 
            {
                bool same = wait->waitedId == waitedId 
                    && wait->nohang == nohang;
                process_assert(same, "notify_waiting({}, nohang={}) called "
                    "after interrupted wait but with different parameters ("
                    "{}, {})", waitedId, nohang, wait->waitedId, wait->nohang);
                debug("({}) merging event for restarted wait call", _pid);
                wait->error = 0;
                return;
            }
        }
    }
    add_event(make_unique<WaitEvent>(*this, waitedId, nohang), true);
}

void Process::notify_failed_wait(int error) 
{
    // search backwards to find the WaitEvent that started the failed wait
    for (size_t i = _events.size() - 1; i >= 0; --i) 
    {
        if (auto wait = dynamic_cast<WaitEvent*>(_events[i].get())) 
        {
            process_assert(wait->error == 0, "notify_failed_wait(\"{}\"): "
                "the previous WaitEvent already failed", strerror_s(error));
            wait->error = error;
            log("{}", wait->to_string());
            return;
        }
    }
    process_assert(false, "notify_failed_wait(\"{}\") couldn't find the "
        "initial wait event that failed", strerror_s(error));
}

void Process::notify_reaped(shared_ptr<Process> child) 
{
    process_assert(child->_state == State::ZOMBIE,
        "notify_reaped({}) called on non-zombie process", child->to_string());
    child->_state = State::REAPED;

    // search backwards to find the WaitEvent that started this wait
    for (size_t i = _events.size() - 1; i >= 0; --i)
    {
        if (auto wait = dynamic_cast<WaitEvent*>(_events[i].get())) 
        {
            process_assert(wait->error == 0, "notify_reaped({}) called when "
                "the last WaitEvent failed", child->to_string());

            // We'll take the successful WaitEvent off our event list and put
            // an ReapEvent there instead (which will contain the WaitEvent).
            // First, put the WaitEvent inside a new unique_ptr.
            unique_ptr<WaitEvent> waitEv(wait);
            // Important! Release the old pointer so the WaitEvent doesn't get
            // free'd when we replace the unique_ptr that currently holds it.
            _events[i].release();
            // Now replace with a ReapEvent that contains the WaitEvent
            _events[i] = make_unique<ReapEvent>(
                *this, std::move(waitEv), std::move(child));

            log("{}", _events[i]->to_string()); // log updated event
            return;
        }
    }
    process_assert(false, "notify_reaped({}) couldn't find the initial wait "
        "event that led to the reapage", child->to_string());
}

void Process::notify_forked(shared_ptr<Process> child) 
{
    // consumeLocation=true (forktrace.h updates source location for forks)
    add_event(make_unique<ForkEvent>(*this, std::move(child)), true);
}

void Process::notify_exec(string file, vector<string> args, int errcode) 
{
    if (_events.empty()) 
    {
        // We have no exec events so far - don't have to worry about merging
        // consumeLocation=true (forktrace.h updates source location for execs)
        add_event(make_unique<ExecEvent>(
            *this, std::move(file), std::move(args), errcode), true);
        return;
    }

    auto event = dynamic_cast<ExecEvent*>(_events.back().get());

    if (!event || event->succeeded() || event->args != args) 
    {
        // the last event wasn't a failed exec event, so don't merge
        add_event(make_unique<ExecEvent>(
            *this, std::move(file), std::move(args), errcode), true);
        return;
    }

    if (get_base_name(file) != get_base_name(event->call().file)
        || event->args != args) 
    {
        // the last exec was for a different program or args - don't merge
        add_event(make_unique<ExecEvent>(
            *this, std::move(file), std::move(args), errcode), true);
        return;
    }

    // Okay, the most recent event by this process was a failed exec for a
    // program of the same name (although maybe in a different directory) and
    // of the same arguments, so we'll merge these together since they are
    // probably just the C library searching $PATH. (If that isn't the case,
    // then no biggie, since the user can still see the history of exec calls
    // if they want to). TODO make sure this feature is actually implemented.
    event->calls.emplace_back(file, errcode); // update existing ExecEvent

    // TODO maybe move printing of location into the event code itself? That
    // would clean some of this up.
    string str = event->call().to_string(*event);
    if (event->location.has_value())
    {
        log("{} @ {}", str, event->location->to_string());
    }
    else
    {
        log("{}", str);
    }
}

void Process::notify_ended(int status) 
{
    // Not really a ProcessTreeError type scenario. People should only call 
    // this function if they have already checked this is the case.
    assert(WIFEXITED(status) || WIFSIGNALED(status));

    if (WIFEXITED(status)) 
    {
        add_event(make_unique<ExitEvent>(*this, WEXITSTATUS(status)));
        // Must set this *after* calling add_event since it only allows events
        // to be added to processes that are State::ALIVE (good).
        _state = State::ZOMBIE;
    } 
    else 
    {
        // If the most recent event was a SignalEvent delivering the same
        // signal, then instead of creating a new SignalEvent, we'll just
        // promote the old one to being a killing signal. TODO lost info?
        if (!_events.empty()) 
        {
            auto event = dynamic_cast<SignalEvent*>(_events.back().get());

            if (event && event->signal == WTERMSIG(status))
            {
                _killed = event->killed = true;
                log("{}", event->to_string());
                _state = State::ZOMBIE;
                return;
            }
        }

        // killed=True since we know this signal ended the process.
        add_event(make_unique<SignalEvent>(*this, WTERMSIG(status), true));
        _state = State::ZOMBIE; // must go after add_event
        _killed = true;
    }
}

void Process::notify_signaled(pid_t sender, int signal) 
{
    // killed=False so far (we don't know if this signal killed yet)
    add_event(make_unique<SignalEvent>(*this, sender, signal, false));
}

/* This is a static member function */
void Process::notify_sent_signal(pid_t killedId, 
                                 Process& source, 
                                 Process* dest, 
                                 int signal, 
                                 bool toThread)
{
    if (dest && (dest != &source) && (dest->pid() == killedId)) 
    {
        // This corresponds to two a signal sent between two distinct processes
        // that are both present in this process tree.
        auto info = make_shared<KillInfo>(source, *dest, signal, toThread);

        // Both processes get a handle to the shared kill information. The 
        // source process consumes their source location (since forktrace.h
        // will update location when kill/tkill/tkill is called).
        source.add_event(make_unique<KillEvent>(source, info, true), true);

        // Some signals like SIGKILL will kill the process instantly, so the 
        // death event will already be there. In that case, we want to put the 
        // kill event before it, so we'll swap them out. In both cases, we
        // add the event directly (eschewing add_event) since we don't want
        // to log the KillEvent twice (the source already did that just above).
        if (dest->dead())
        {
            assert(!dest->_events.empty());
            unique_ptr<Event>& deathEvent = dest->_events.back();
            dest->_events.push_back(
                make_unique<KillEvent>(*dest, std::move(info), false));
            swap(deathEvent, dest->_events.back());
        } 
        else 
        {
            dest->_events.push_back(
                make_unique<KillEvent>(*dest, move(info), false));
        }
    } 
    else 
    {
        // We're not able to draw a clean line between two processes in the
        // tree, so we'll just use a RaiseEvent instead.
        source.add_event(
            make_unique<RaiseEvent>(source, killedId, signal, toThread), true);
    }
}

void Process::notify_orphaned() 
{
    process_assert(_state == State::ZOMBIE, "notify_orphaned() called on "
        " a process that wasn't a ZOMBIE");
    _state = State::ORPHANED;
}

void Process::update_location(SourceLocation location) 
{
    debug("{} got updated location {}", _pid, location.to_string());
    _location.emplace(std::move(location));
}

string Process::to_string() const 
{
    return format("{} {}", _pid, command_line());
}

void Process::print_tree(Indent indent) const 
{
    std::cerr << format("{}process {}\n", indent, _pid);
    for (const auto& event : _events) 
    {
        event->print_tree(indent + 1);
    }
}

string_view Process::state() const 
{
    switch (_state) 
    {
        case State::ALIVE:      return "alive";
        case State::ZOMBIE:     return "zombie";
        case State::REAPED:     return "reaped";
        case State::ORPHANED:   return "orphaned";
    }
    assert(!"Unreachable");
}

string Process::command_line(int eventIndex) const 
{
    if (const ExecEvent* lastExec = most_recent_exec(eventIndex))
    {
        const vector<string>& args = lastExec->args;
        return format("{} [ {} ]", lastExec->call().file, join(args));
    }
    else
    {
        return format("{} [ {} ]", _initialName, join(_initialArgs));
    }
}

const Event& Process::death_event() const 
{
    assert(dead() && !_events.empty());
    return *_events.back().get();
}
