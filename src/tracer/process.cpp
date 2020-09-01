#include <cassert>
#include <sstream>

#include "process.hpp"
#include "system.hpp"
#include "terminal.hpp"
#include "util.hpp"

using namespace std;

/* Global configuration option. Exposed globally via setExecMergingEnabled. */
static bool mergeExecs = true;

Process::Process(pid_t pid, const shared_ptr<Process>& parent)
    : _pid(pid), _parent(parent), _state(State::ALIVE), _killed(false)
{
    const ExecEvent* const lastExec = parent->mostRecentExec();
    if (!lastExec) {
        _initialName = parent->_initialName;
        _initialArgs = parent->_initialArgs;
    } else {
        _initialName = lastExec->file();
        _initialArgs = lastExec->args;
    }
}

/* Will do a reverse search to find the most recent exec event for this
 * process, and will return null if it couldn't be found. The pointer will
 * become invalid if the event is removed from our list. If startIndex is
 * negative, then we'll search all events. Otherwise, we'll start the reverse
 * search from the event preceding the specified index. */
const ExecEvent* const Process::mostRecentExec(int startIndex) const {
    if (_events.empty() || startIndex == 0) {
        return nullptr;
    }
    if (startIndex < 0) {
        startIndex = _events.size();
    }
    startIndex--;
    assert(0 <= startIndex && startIndex < _events.size());

    for (long i = startIndex; i >= 0; --i) {
        if (auto exec = dynamic_cast<const ExecEvent*>(_events.at(i).get())) {
            if (exec->succeeded()) {
                return exec;
            }
        }
    }

    return nullptr;
}

/* A version of addEvent that doesn't log anything to stdout. */
void Process::addEventSilent(unique_ptr<Event> event, bool consumeLocation) {
    assert(_state == State::ALIVE);
    if (consumeLocation) {
        event->setLocation(move(_location));
    }
    _events.push_back(move(event));
}

/* Add this event to the list and log it out. An assertion will fail if
 * this process has already ended. If `consumeLocation` is true, then the
 * current source location is moved into the provided event if it exists. */
void Process::addEvent(unique_ptr<Event> event, bool consumeLocation) {
    assert(_state == State::ALIVE);
    tlog << event->toString();
    if (_location && consumeLocation) {
        tlog << " @ " << _location->toString();
        event->setLocation(move(_location));
    }
    tlog << endl;
    _events.push_back(move(event));
}

void Process::notifyWaiting(pid_t waitedId, bool nohang) {
    // If the very last event was a failed wait event with ERESTARTSYS, then
    // we'll just merge the two together (we only really care about showing
    // them separately when another event appears in between them).
    if (!_events.empty()) {
        if (auto wait = dynamic_cast<WaitEvent*>(_events.back().get())) {
            if (wait->error == ERESTARTSYS) {
                assert(wait->waitedId == waitedId && wait->nohang == nohang);
                dbg << "Merging event for restarted wait call" << endl;
                wait->error = 0;
                return;
            }
        }
    }
    addEvent(make_unique<WaitEvent>(*this, waitedId, nohang), true);
}

void Process::notifyFailedWait(int error) {
    for (auto& event : reverse(_events)) {
        if (auto wait = dynamic_cast<WaitEvent*>(event.get())) {
            assert(wait->error == 0);
            wait->error = error;
            tlog << wait->toString() << endl;
            return;
        }
    }
    assert(!"Unreachable");
}

void Process::notifyReaped(shared_ptr<Process> child) {
    assert(child->_state == State::ZOMBIE);
    child->_state = State::REAPED;
    for (auto& event : reverse(_events)) {
        if (auto wait = dynamic_cast<WaitEvent*>(event.get())) {
            assert(wait->error == 0);
            unique_ptr<WaitEvent> waitEv(wait);
            event.release(); // must .release() first!!! (so it doesn't free)
            event = make_unique<ReapEvent>(*this, move(waitEv), move(child));
            tlog << event->toString() << endl; // log updated event
            return;
        }
    }
    assert(!"Unreachable");
};

void Process::notifyForked(shared_ptr<Process> child) {
    addEvent(make_unique<ForkEvent>(*this, move(child)), true);
}

void Process::notifyExec(string file, vector<string> args, int errcode) {
    if (_events.empty() || !mergeExecs) {
        addEvent(make_unique<ExecEvent>(*this, move(file), move(args), 
                errcode), true);
        return;
    }

    auto event = dynamic_cast<ExecEvent*>(_events.back().get());

    if (!event || event->succeeded() || event->args != args) {
        // the last event wasn't a failed exec event, so don't merge
        addEvent(make_unique<ExecEvent>(*this, move(file), move(args), 
                errcode), true);
        return;
    }

    assert(!event->calls.empty());

    if (getBaseName(file) != getBaseName(event->calls.back().file)
            || event->args != args) {
        // the last exec was for a different program or args - don't merge
        addEvent(make_unique<ExecEvent>(*this, move(file), move(args), 
                errcode), true);
        return;
    }

    // Okay, the most recent event by this process was a failed exec for a
    // program of the same name (although maybe in a different directory) and
    // of the same arguments, so we'll merge these together since they are
    // probably just the C library searching $PATH. (If that isn't the case,
    // then no biggie, since the user can still see the history of exec calls).
    event->calls.emplace_back(file, errcode);
    tlog << event->calls.back().toString(*event) << endl;
}

void Process::notifyEnded(int status) {
    assert(WIFEXITED(status) || WIFSIGNALED(status));

    if (WIFEXITED(status)) {
        addEvent(make_unique<ExitEvent>(*this, WEXITSTATUS(status)));
        _state = State::ZOMBIE; // must set this *after* calling addEvent
    } else {
        // If the most recent event was a SignalEvent delivering the same
        // signal, then instead of creating a new SignalEvent, we'll just
        // promote the old one to being a killing signal.
        if (!_events.empty()) {
            auto event = dynamic_cast<SignalEvent*>(_events.back().get());

            if (event && event->signal == WTERMSIG(status)) {
                _killed = event->killed = true;
                _state = State::ZOMBIE;
                tlog << event->toString() << endl;
                return;
            }
        }

        addEvent(make_unique<SignalEvent>(*this, WTERMSIG(status), true));
        _state = State::ZOMBIE;
        _killed = true;
    }
}

void Process::notifySignaled(pid_t sender, int signal) {
    addEvent(make_unique<SignalEvent>(*this, sender, signal, false));
}

/* This is a static member function */
void Process::notifySentSignal(pid_t killedId, Process& source, 
        Process* dest, int signal, bool toThread)
{
    if (dest && (dest != &source) && (dest->pid() == killedId)) {
        // This corresponds to two a signal sent between two distinct processes
        // that are both present in this process tree. Make sure to add one
        // instance of the event silently, so we don't log it twice.
        auto info = make_shared<KillInfo>(source, *dest, signal, toThread);
        source.addEvent(
                make_unique<KillEvent>(source, info, true), true);
        dest->addEventSilent(
                make_unique<KillEvent>(*dest, move(info), false), true);
    } else {
        // We're not able to draw a clean line between two processes in the
        // tree, so we'll just use a RaiseEvent instead.
        source.addEvent(
                make_unique<RaiseEvent>(source, killedId, signal, toThread), 
                true);
    }
}

void Process::notifyOrphaned() {
    assert(_state == State::ZOMBIE);
    _state = State::ORPHANED;
}

void Process::updateLocation(unique_ptr<SourceLocation> location) {
    assert(!_location); // TODO asserting on (effectively) user input is dodge
    vdbg << _pid << " got updated location: " << location->toString() << endl;
    _location = move(location);
}

string Process::toString() const {
    ostringstream oss;
    oss << _pid << ' ' << commandLine();
    return oss.str();
}

void Process::printTree(Indent indent) const {
    tlog << indent << "process " << _pid << endl;
    for (const auto& event : _events) {
        event->printTree(indent + 1);
    }
}

string_view Process::state() const {
    switch (_state) {
        case State::ALIVE:
            return "alive";
        case State::ZOMBIE:
            return "zombie";
        case State::REAPED:
            return "reaped";
        case State::ORPHANED:
            return "orphaned";
    }
    assert(!"Unreachable");
}

string Process::commandLine(int eventIndex) const {
    ostringstream oss;
    const vector<string>* args;
    const ExecEvent* const lastExec = mostRecentExec(eventIndex);

    if (!lastExec) {
        oss << _initialName;
        args = &_initialArgs;
    } else {
        oss << lastExec->file();
        args = &lastExec->args;
    }

    oss << " [ ";
    for (auto& arg : *args) {
        oss << arg << ' ';
    }
    oss << ']';
    return oss.str();
}

const Event& Process::deathEvent() const {
    assert(dead());
    assert(!_events.empty());
    return *_events.back().get();
}

void setExecMergingEnabled(bool enabled) {
    mergeExecs = enabled;
}
