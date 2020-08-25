#include <string>
#include <unistd.h>
#include <sys/ptrace.h>
#include <cassert>
#include <cstring>
#include <system_error>
#include <sstream>

#include "tracer.h"
#include "tracee.h"
#include "process.h"
#include "util.h"

using namespace std;

/* Same as the public member function removeTracee except it accepts an
 * iterator. Returns the iterator returned by unordered_map::erase so that
 * this can be used when iterating over maps. Will update _numStopped. */
auto Tracer::removeTracee(decltype(_tracees)::iterator pair) {
    assert(pair != _tracees.end());
    pid_t pid = pair->second->pid();

    dbg << "Erasing " << pid << " (numStopped=" << _numStopped << '/'
        << _tracees.size() << ") pgid: " << pair->second->pgid() << endl;

    if (pair->second->stopped()) {
        _numStopped--;
    }
    auto it = _tracees.erase(pair);

    dbg << "Erased " << pid << " (numStopped=" << _numStopped << '/'
        << _tracees.size() << ')' << endl;

    if (_tracees.empty()) {
        _leader = -1;
    }

    return it;
}

/* Will check if the specified iterator into _tracees refers to a dead
 * immediate child of ours, and if it is, then it is removed from the map,
 * `it` is set to the iterator to the next element, and false is returned.
 * Otherwise, true is returned. Updates _numStopped. */
bool Tracer::removeIfDeadChild(decltype(_tracees)::iterator& it) {
    // If it's our child and it's dead, then remove it
    if (it->second->dead() && it->first == _leader) {
        dbg << "Child group leader " << _leader << " died" << endl;
        it = removeTracee(it);
        return true;
    }
    return false;
}

/* Check via the callback if there are currently any orphans available for 
 * us. If we find any, then we'll call removeTracee on them (so don't call
 * inside a loop when iterating over the map). Will invalidate any current
 * iterators into the _tracees map. */
void Tracer::collectOrphans() {
    pid_t pid;

next_orphan:
    while (_getOrphanCallback(pid)) {
        auto& pids = _recycledPIDs;
        for (auto it = pids.begin(); it != pids.end(); ++it) {
            if (*it == pid) {
                pids.erase(it);
                goto next_orphan; // it's already been removed, so skip
            }
        }

        auto pair = _tracees.find(pid);
        assert(pair != _tracees.end());
        pair->second->process()->notifyOrphaned();
        removeTracee(pair);
        tlog << pid << " orphaned" << endl;
    }
}

void Tracer::addTracee(unique_ptr<Tracee> t) {
    assert(t->pgid() == _leader);
    pid_t pid = t->pid();
    auto pair = _tracees.find(pid);
    if (pair != _tracees.end()) {
        // The fact that the tracee is already in the list means that its PID
        // has been recycled (this can happen when the reaper reaps it, but
        // the PID is recycled before we get notified about it).
        pair->second->process()->notifyOrphaned();
        _recycledPIDs.push_back(pid);
        removeTracee(pair);
        tlog << pid << " orphaned" << endl;
    }

    if (t->stopped()) {
        _numStopped++;
    }
    _tracees.emplace(pid, move(t));

    dbg << "Added " << pid << " (numStopped=" << _numStopped << '/'
        << _tracees.size() << ')' << endl;

    if (_state == State::RUNNING) {
        auto it = _tracees.find(pid);
        assert(it != _tracees.end());
        resumeTracee(it);
    }
}

void Tracer::removeTracee(pid_t pid) {
    removeTracee(_tracees.find(pid));
    assert(_tracees.find(pid) == _tracees.end());
}

const Tracee& Tracer::operator[](pid_t pid) {
    assert(_tracees.find(pid) != _tracees.end());
    return *_tracees[pid].get();
}

shared_ptr<Process> Tracer::findNode(pid_t pid) {
    auto it = _tracees.find(pid);
    if (it == _tracees.end()) {
        return nullptr;
    }
    return it->second->process();
}
 
/* Kill the specified PID with SIGKILL and reap it so that it's not a zombie.
 * If the PID is not our child, then this function fails silently. If some
 * other error occurs, then an exception is thrown. PRESERVES ERRNO!!!!. */
void killAndReap(pid_t pid) {
    int e = errno;

    if (kill(pid, SIGKILL) == -1 && errno != ESRCH) {
        throw system_error(errno, generic_category(), "kill");
    }
    
    // Just keep looping until process disappears (causing waitpid to fail).
    while (waitpid(pid, nullptr, 0) != -1) { }

    errno = e;
}

/* Helper function for Tracer::start. Called when the tracee did not stop with
 * SIGSTOP as expected. We take the status that waitpid returned for the tracee
 * and throw an exception to report the tracee's error. `name` specifies the
 * name of the system call that would have failed. ALWAYS THROWS EXCEPTION. */
void throwFailedStart(pid_t pid, int status, const string& name) {
    if (WIFEXITED(status)) {
        throw system_error(WEXITSTATUS(status), generic_category(), name);
    }
    if (WIFSIGNALED(status)) {
        throw runtime_error("Tracee killed by unexpected signal.");
    }

    /* If we're here, the tracee hasn't died yet - so make sure of it. */
    killAndReap(pid);

    if (WIFSTOPPED(status)) {
        throw runtime_error("Tracee stopped by unexpected signal.");
    }
    throw runtime_error("Unexpected change of state by tracee.");
}

/* Helper function for Tracer::start. Performs the actual startup sequence for
 * the tracee and configures it to be traced correctly. This will throw system
 * errors according to the causes of failure in the child process. */
pid_t startTracee(const char** childArgs) {
    pid_t pid = fork();
    switch (pid) {
    case -1:
        throw system_error(errno, generic_category(), "fork");
    case 0:
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1) {
            assert(0 <= errno && errno <= 255);
            _exit(errno); // hacky way to send errno to parent
            /* NOTREACHED */
        }

        raise(SIGSTOP); // sync up with tracer

        if (setpgid(0, 0) == -1) {
            assert(0 <= errno && errno <= 255);
            _exit(errno);
            /* NOTREACHED */
        }

        raise(SIGSTOP); // sync up with tracer

        execvp(childArgs[0], const_cast<char* const*>(childArgs));
        _exit(1); // the tracer will learn cause of failure later with ptrace
        /* NOTREACHED */
    default:
        break;
    }

    /* Ensure that ptrace(PTRACE_TRACEME, ...) succeeded, then continue the
     * tracee for the next step in the sequence. */
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        throw system_error(errno, generic_category(), "waitpid");
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        throwFailedStart(pid, status, "ptrace"); // reaps for us
        /* NOTREACHED */
    }
    if (ptrace(PTRACE_CONT, pid, 0, 0) == -1) {
        killAndReap(pid); // preserves errno
        throw system_error(errno, generic_category(), "ptrace");
    }

    /* Ensure that the setpgid call succeeded and then configure the options
     * required by the Tracee class. We'll then leave the tracee in a stopped
     * status for the caller to resume when ready. */
    if (waitpid(pid, &status, 0) == -1) {
        throw system_error(errno, generic_category(), "waitpid");
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        throwFailedStart(pid, status, "setpgid"); // reaps for us
        /* NOTREACHED */
    }
    if (ptrace(PTRACE_SETOPTIONS, pid, 0, Tracee::getTraceOptions()) == -1) {
        killAndReap(pid); // preserves errno
        throw system_error(errno, generic_category(), "ptrace");
    }

    return pid;
}

shared_ptr<Process> Tracer::start(const char** childArgs) {
    assert(childArgs && *childArgs);
    if (_leader != -1) {
        throw runtime_error("Tracer::start: an old session is still active.");
    }
    
    pid_t pid = startTracee(childArgs);

    string file;
    if (!_tracerArgs.empty()) {
        file = _tracerArgs.front();
    }

    // Tracee's destructor ensures that the process is cleaned up if an error
    // condition occurs (will SIGKILL it and wait(2) for it until it shuts up).
    // Also note that startTracee should have put the tracee in its own process
    // group, so its PGID should equal its PID. 
    auto t = make_unique<Tracee>(*this, pid, pid, file, _tracerArgs); 
    shared_ptr<Process> p = t->process();

    try {
        while (!t->execed()) {
            t->tryResume();

            if (t->dead()) {
                throw runtime_error("Tracee was not able to exec.");
            }

            int status;
            if (waitpid(pid, &status, 0) == -1) {
                throw system_error(errno, generic_category(), "waitpid");
            }

            t->handleWaitNotification(status);
        }
    } catch (const BadTraceError& e) {
        e.print(tlog);
        throw runtime_error("Tracee was not able to exec.");
    }
    
    _leader = pid;
    addTracee(move(t));
    return p;
}

shared_ptr<Process> Tracer::start(const vector<string>& childArgs) {
    assert(!childArgs.empty());
    vector<const char*> argv;
    argv.reserve(childArgs.size() + 1);

    for (size_t i = 0; i < childArgs.size(); ++i) {
        argv.push_back(childArgs[i].c_str());
    }

    argv.push_back(nullptr);
    return start(argv.data());
}

/*auto Tracer::handleBadTrace(decltype(_tracer)::iterator it) {
    // TODO check via callback whether we want to SIGKILL the tracee or just
    // detach from it. To support detaching, I have to make the process tree
    // more flexible so that it can handle interacting with PIDs that aren't
    // being traced. I guess if the detached tracee has a parent inside the
    // process tree, we can keep it there until it gets reaped with question
    // marks along its path...?
    assert(!"Not implemented.");
}*/

/* Given whether a tracee was stopped before and after some operation, this
 * updates the current tally of how many tracees are stopped. */
void Tracer::updateStopCount(decltype(_tracees)::iterator it, 
        bool stoppedBefore, bool stoppedAfter) 
{
    if (stoppedBefore) {
        if (!stoppedAfter) {
            _numStopped--;
            dbg << it->first << " resumed (numStopped=" << _numStopped << '/'
                << _tracees.size() << ')' << endl;
        }
    } else {
        if (stoppedAfter) {
            _numStopped++;
            dbg << it->first << " stopped (numStopped=" << _numStopped << '/'
                << _tracees.size() << ')' << endl;
        }
    }
}

/* Resumes the tracee, updating the _numStopped count to reflect it. Will
 * return false if the tracee is stopped after trying to resume it. Will
 * handle exceptions raised by the tracee. */
bool Tracer::resumeTracee(decltype(_tracees)::iterator it) {
    bool stoppedBefore = it->second->stopped();
    try {
        it->second->tryResume();
    } catch (const BadTraceError& e) {
        e.print(tlog);
        assert(!"Can't handle bad traces yet");
    } catch (const exception& e) {
        tlog << "Exception raised by tracee " << it->first << endl
            << "  what():  " << e.what() << endl;
        assert(!"Can't handle bad traces yet");
    }
    bool stoppedAfter = it->second->stopped();
    updateStopCount(it, stoppedBefore, stoppedAfter);
    return stoppedAfter;
}

/* Resumes all tracees and after resuming each one, checks to see if the
 * tracee is a dead child, and if it is, it removes it. Also makes sure
 * to catch and handle exceptions raised by the tracee. */
void Tracer::resumeAllTracees() {
    for (auto it = _tracees.begin(); it != _tracees.end(); /* */) {
        resumeTracee(it);
        if (!removeIfDeadChild(it)) { // see function comment
            it++; // only increment when `it` isn't erased
        }
    }
}

/* Call this when we get a wait notification for a tracee. It will pass
 * the status to the Tracee so it can handle it, and update the _numStopped
 * count, and also make sure to handle exceptions from tracee. */
void Tracer::handleWaitNotification(decltype(_tracees)::iterator it, 
        int status) 
{
    bool stoppedBefore = it->second->stopped();
    try {
        it->second->handleWaitNotification(status, _haltSent);
    } catch (const BadTraceError& e) {
        e.print(tlog);
        assert(!"Can't handle bad traces yet");
    } catch (const exception& e) {
        tlog << "Exception raised by tracee " << it->first << endl
            << "  what():  " << e.what() << endl;
        assert(!"Can't handle bad traces yet");
    }
    updateStopCount(it, stoppedBefore, it->second->stopped());
}

void Tracer::go() {
    _haltSent = false; // TODO memory fence
    _state = State::RUNNING; // TODO memory fence?
    resumeAllTracees();
    collectOrphans();

    int status;
    pid_t pid;
    while ((pid = wait(&status)) != -1) {
        auto it = _tracees.find(pid);
        assert(it != _tracees.end());

        handleWaitNotification(it, status);
        if (!_haltSent) {
            resumeTracee(it);
        }
        removeIfDeadChild(it); // XXX: TODO xxx? `it` might be modified by this
        collectOrphans(); // iterator invalidated by this

        // TODO what if _haltSent set to true at an awkward moment?
        if (_haltSent && _numStopped == _tracees.size()) { // why && _haltSent?
            dbg << "The run is over" << endl;
            _state = State::STOPPED;
            return;
        }
    }
    if (errno != ECHILD) {
        throw system_error(errno, generic_category(), "wait");
    }
    _state = State::STOPPED;
    
    while (!_tracees.empty()) {
        collectOrphans();
    }
}

bool Tracer::march() {
    _haltSent = false;
    _state = State::MARCHING; // TODO memory fence?
    resumeAllTracees();
    collectOrphans();

    int status;
    pid_t pid;
    while ((pid = wait(&status)) != -1) {
        auto it = _tracees.find(pid);
        assert(it != _tracees.end());

        handleWaitNotification(it, status);
        removeIfDeadChild(it); // might modify `it`
        collectOrphans(); // iterator also invalid after this

        if (_numStopped == _tracees.size()) {
            _state = State::STOPPED;
            return true;
        }
    }
    if (errno != ECHILD) {
        throw system_error(errno, generic_category(), "wait");
    }
    _state = State::STOPPED;
    
    while (!_tracees.empty()) {
        collectOrphans();
    }
    return false;
}

void Tracer::step(pid_t pid) {
    auto it = _tracees.find(pid);
    if (it == _tracees.end()) {
        throw runtime_error("No tracee has the specified PID.");
    }

    _haltSent = false; // TODO check race conditions with _haltSent
    _continuedPID = pid;
    _state = State::STEPPING; // TODO memory fence

    if (!resumeTracee(it) || removeIfDeadChild(it)) {
        _state = State::STOPPED;
        return;
    }
    collectOrphans();

    int status;
    while (waitpid(it->first, &status, 0) != -1) {
        handleWaitNotification(it, status);
        if (removeIfDeadChild(it) || it->second->stopped()) { // order matters
            _state = State::STOPPED;
            return;
        }
    }
    if (errno != ECHILD) {
        throw system_error(errno, generic_category(), "wait");
    }
    _state = State::STOPPED;
    collectOrphans();
}

void Tracer::nuke() {
    setLogEnabled(false);
    //tlog << "HELLO!" << flush << endl;
    signalAll(SIGKILL);
    go();
    assert(_tracees.empty());
    assert(_recycledPIDs.empty()); // TODO ??
    assert(_state == State::STOPPED);
    assert(_leader == -1);
    assert(_numStopped == 0);
    setLogEnabled(true);
}

void Tracer::halt() {
    if (_haltSent || _state == State::STOPPED) {
        return;
    }

    if (_state == State::STEPPING) {
        kill(_continuedPID, getHaltSignal());
        tlog << "Sent " << getSignalName(getHaltSignal()) << '(' 
            << getHaltSignal() << ") to " << _continuedPID << endl;
    } else {
        signalAll(getHaltSignal());
    }

    // TODO does C++ memory model guarantee this will be visible
    _haltSent = true; // TODO RACE CONDITIONS 
}

void Tracer::signalAll(int signal) {
    if (_leader == -1) {
        return;
    }
    if (killpg(_leader, signal) == -1) {
        throw system_error(errno, generic_category(), "killpg");
    }
}

void Tracer::killAll() {
    signalAll(SIGKILL);
}

void Tracer::printList() const {
    for (auto& pair : _tracees) {
        tlog << pair.first << ' ' << pair.second->state() << ' ' 
            << pair.second->process()->commandLine() << endl;
    }
    tlog << "total: " << _tracees.size() << endl;
}
