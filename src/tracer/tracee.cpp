#include <system_error>
#include <sstream>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <sys/ptrace.h>

#include "process.hpp"
#include "system.hpp"
#include "tracer.hpp"
#include "tracee.hpp"
#include "util.hpp"

using namespace std;

BadTraceError::BadTraceError(pid_t pid, int err, string func) : _pid(pid) {
    ostringstream oss;
    oss << func << ' ' << strerror(err);
    _message = oss.str();
}

void BadTraceError::print(std::ostream& os) const {
    os << "Got a BadTraceError. This means one of four things:" << endl
        << "  (1) Kernel bug (this has happened to me!)" << endl
        << "  (2) I haven't read the man pages carefully enough" << endl
        << "  (3) There is a regular ole' bug in this program :-(" << endl
        << "  (4) A third party has sabotaged us!!!" << endl;
    os << "Error summary:" << endl
        << "  what():  " << what() << endl;
}

/* A sub-class of BlockingCall specialised for wait calls (wait4 or waitid).
 * This class does a bunch of trickery to obtain the result of the wait call,
 * even when the tracee program specifies 'NULL' as the argument to the result
 * value. What we have to do in that case is pick some random readable/writable
 * address in the tracees memory space, and use that for the result instead (we
 * then have to modify the system call to point to that instead. 
 *
 * Template arguments:
 *
 *  Result : The type of the result, e.g., int or siginfo_t.
 *
 *  ZeroTheResult : Whether we should zero out the result memory before passing
 *      it into the wait call.
 *
 *  ResultArgIndex : The index of the argument that points to where the result
 *      should be written.
 */
template <class Result, bool ZeroTheResult, int ResultArgIndex>
class WaitCall : public BlockingCall {
private:
    pid_t _waitedId; // same meaning as pid argument of waitpid(2)
    bool _nohang; // does this have WNOHANG?
    Result* _result; // address in tracee's memory space
    unique_ptr<Result> _oldData; // backup of the old data at _result.

protected:
    WaitCall(pid_t target, Result* result, int flags) 
        : _waitedId(target), _nohang(flags & WNOHANG), _result(result) { }

    /* Prepare the wait call, modifying its parameters in the event that the
     * user didn't provide us with a location to store the result.
     *
     * Cleanup:
     *  If false is returned, then reaping the tracee is left to the caller.
     */
    virtual bool prepare(Tracee& tracee);

    /* Implementation for WaitCall to get a description of this blocking call*/
    virtual string_view verb() const { return "waiting"; }

    /* Retrieve the result and return value of the wait call. Return false if
     * the tracee died and throw an exception if some error occurred. */
    bool getResult(pid_t pid, Result& result, size_t& retval);

    /* Calling these will update the process tree if necessary */
    void onSuccess(Tracee& tracee, pid_t reaped);
    void onFailure(Tracee& tracee, int error);
};

class Wait4Call : public WaitCall<int, false, 1> {
public:
    Wait4Call(pid_t pid, int* status, int flags) 
        : WaitCall<int, false, 1>(pid, status, flags) { }

    virtual bool finalise(Tracee& tracee);
};

/* Converts the arguments used by waitid to the pid argument used by wait4.
 * If the arguments are invalid, then we'll just make something up (it won't
 * end up mattering since the wait call will just return with EINVAL). */
pid_t toWait4ID(idtype_t type, id_t id) {
    switch (type) {
        case P_ALL:
            return -1;
        case P_PID:
            return id;
        case P_PGID:
            return -(pid_t)id;
        default:
            return numeric_limits<pid_t>::max(); // clearly wrong
    }
}

class WaitIDCall : public WaitCall<siginfo_t, true, 2> {
public:
    WaitIDCall(idtype_t type, id_t id, siginfo_t* infop, int flags) 
        : WaitCall<siginfo_t, true, 2>(toWait4ID(type, id), infop, flags) { }

    virtual bool finalise(Tracee& t);
};

template <class Result, bool ZeroTheResult, int ResultArgIndex>
bool WaitCall<Result, ZeroTheResult, ResultArgIndex>::prepare(Tracee& tracee) {
    pid_t pid = tracee.pid();
    if (_result == nullptr) {
        // The tracee specified NULL for the address of the result, so find
        // some block of memory in the tracee that we can use to store the
        // syscall's result.
        if (!getTraceeResultAddr(pid, (void*&)_result)) {
            return false;
        }

        // Save the old data that was stored at that address.
        _oldData = make_unique<Result>();
        try {
            if (!copyFromTracee(pid, _oldData.get(), _result, 
                    sizeof(Result))) {
                return false;
            }
        } catch (const system_error& e) {
            // intercept EFAULT/EIO, which will occur if the tracee specifies
            // a bad memory address as the result argument for the wait call.
            // If that has happened, then we'll indicate that by setting both
            // _oldData and _result to null. Then when we come to finalise the
            // wait call, we'll just let it fail.
            if (e.code().value() != EFAULT && e.code().value() != EIO) {
                throw e;
            }
            _oldData.reset();
            _result = nullptr;
            return true;
        }

        // Now change the syscall argument to point to this block of memory.
        if (!setSyscallArg(pid, (size_t)_result, ResultArgIndex)) {
            return false;
        }
    }
    
    if (ZeroTheResult && !memsetTracee(pid, _result, 0, sizeof(Result))) {
        return false; 
    }

    // Now we notify the process tree that the wait has begun!
    tracee.process()->notifyWaiting(_waitedId, _nohang);
    return true;
}

template <class Result, bool ZeroTheResult, int ResultArgIndex>
bool WaitCall<Result, ZeroTheResult, ResultArgIndex>::getResult(pid_t pid, 
        Result& result, size_t& retval) 
{
    // If _result and _oldData are null, then that indicates that the address
    // specified by the tracee for the result is invalid and thus the system
    // call will fail with a memory fault.
    if (_oldData == nullptr && _result == nullptr) {
        retval = -1; // what wait calls return on error
        return true;
    }

    // Retrieve the return value of the syscall
    if (!getSyscallRet(pid, retval)) {
        return false;
    }

    // Retrieve the result of the wait call
    if (!copyFromTracee(pid, &result, _result, sizeof(Result))) {
        return false;
    }

    if (_oldData != nullptr) {
        // If _oldData isn't null, then that means we had to pick a random
        // address in the tracee's memory. Let's restore that.
        if (!copyToTracee(pid, _result, _oldData.get(), sizeof(Result))) {
            return false;
        }

        // Restore the syscall arg just to be safe. Might not be necessary
        // depending on the syscall calling convention for this architecture.
        if (!setSyscallArg(pid, 0, ResultArgIndex)) {
            return false;
        }
    }

    return true;
}

template <class Result, bool ZeroTheResult, int ResultArgIndex>
void WaitCall<Result, ZeroTheResult, ResultArgIndex>
::onSuccess(Tracee& tracee, pid_t chosen)
{
    shared_ptr<Process> child = tracee.tracer()[chosen].process();
    tracee.process()->notifyReaped(move(child));
    tracee.tracer().removeTracee(chosen);
}

template <class Result, bool ZeroTheResult, int ResultArgIndex>
void WaitCall<Result, ZeroTheResult, ResultArgIndex>
::onFailure(Tracee& tracee, int error)
{
    tracee.process()->notifyFailedWait(error);
}

bool Wait4Call::finalise(Tracee& tracee) {
    int status;
    size_t retval;
    if (!getResult(tracee.pid(), status, retval)) {
        return false;
    }
    if ((pid_t)retval > 0 && (WIFEXITED(status) || WIFSIGNALED(status))) {
        onSuccess(tracee, retval);
    } else if ((pid_t)retval < 0) {
        onFailure(tracee, -(int)retval);
    }
    return true;
}

bool WaitIDCall::finalise(Tracee& tracee) {
    siginfo_t info;
    size_t retval;
    if (!getResult(tracee.pid(), info, retval)) {
        return false;
    }
    // waitid will return 0 on success. However, this includes if WNOHANG was
    // specified and nothing happened, so we check info.si_pid to see if the
    // child actually changed state (we zero it out beforehand to be sure).
    if (retval == 0 && info.si_pid != 0
            && (info.si_code == CLD_EXITED
            || info.si_code == CLD_KILLED
            || info.si_code == CLD_DUMPED)) 
    {
        onSuccess(tracee, info.si_pid);
    } else if ((int)retval < 0) {
        onFailure(tracee, -(int)retval);
    }
    return true;
}

/* Call this when we've reached the syscall-exit-stop for a failed fork
 * call. If the fork failed due to the delivery of an interrupting signal,
 * then the failure will be ignored. For any other cause of failure, this 
 * function will exit the program - terminating all tracees (due to the
 * PTRACE_O_EXITKILL option). This function won't set _syscall back to 
 * SYSCALL_NONE, so the caller must do that for it. */
void Tracee::handleFailedFork() {
    /* We've reached a syscall-exit-stop for the fork call, let's check the
     * return value and determine the cause of failure. */
    size_t retval;
    if (!getSyscallRet(_pid, retval)) {
        expectEnded();
        return;
    }
    
    int err = -(long)retval;
    if (err == ERESTARTNOINTR) { // see man 2 fork
        /* The fork call has been interrupted by the delivery of a signal (this
         * error is only visible to ptracers). We'll just return - the tracee
         * will then retry the fork when it next hits syscall-entry-stop, in
         * which case we'll get another go at this. */
        tlog << _pid << " fork interrupted (to be resumed)" << endl;
        resume();
        return;
    }

    /* If the fork failed due to any other reason than an interrupting signal,
     * then just kill everything and give up. This is basically a built-in
     * protection against people fork-bombing themselves. (Note that just the
     * act of us exiting will kill all the tracees, since we should have set
     * them up with the PTRACE_O_EXITKILL option). */
    // TODO strerror isn't thread safe??? (what the FUCK C?????) WHAT IS
    // YOUR FUCKING PROBLEM WITH HAVING HIDDEN GLOBAL STATE IN ALL OF YOUR
    // FUCKING LIBRARY FUNCTIONS. FUUUUUUUUUCCCK
    tlog << _pid << " failed fork: " << strerror(err) << endl;
    tlog << "Nuking everything with SIGKILL..." << endl;
    exit(1);
    /* NOTREACHED */
}

/* Also called for fork-like clones. Doesn't work with vfork yet :-( */
void Tracee::handleFork() {
    int status;
    if (!resume() || !waitForStop(status)) {
        return;
    }

    if (!IS_FORK_EVENT(status)) {
        if (!IS_SYSCALL_EVENT(status)) {
            throw BadTraceError(_pid,
                    "Expected syscall-exit-stop after bad fork.");
        }
        _syscall = SYSCALL_NONE;
        handleFailedFork();
        return;
    }

    unsigned long childId;
    if (ptrace(PTRACE_GETEVENTMSG, _pid, 0, (void *)&childId) == -1) {
        if (errno == ESRCH) {
            expectEnded();
            return;
        }
        throw BadTraceError(_pid, errno, "ptrace");
    }

    /* `false` specifies that the tracee is currently not stopped. */
    auto child = make_unique<Tracee>(_tracer, childId, _pgid, _process, false);
    _process->notifyForked(child->process());

    /* We expect SIGSTOP, unless the child was abruptly killed. */
    if (child->waitForStop(status) && !child->dead() 
            && WSTOPSIG(status) != SIGSTOP) {
        /* Still add it on failure, since it needs to be reaped. */
        _tracer.addTracee(move(child)); // TODO ??
        throw BadTraceError(childId,
                "Expected SIGSTOP from newly forked tracee.");
    }
    _tracer.addTracee(move(child)); // tracer might resume child if it wants
    
    if (!resume() || !waitForStop(status)) {
        return;
    }
    if (!IS_SYSCALL_EVENT(status)) {
        throw BadTraceError(_pid, "Expected syscall-exit-stop after fork.");
    }
    _syscall = SYSCALL_NONE;
}

void Tracee::handleExec(const char* filename, const char** argv) {
    vector<string> args;
    string file;
    try {
        if (!copyArgsFromTracee(_pid, args, argv)
                || !copyStringFromTracee(_pid, file, filename)) {
            expectEnded();
            return;
        }
    } catch (const system_error& e) {
        // Intercept EFAULT/EIO - which will occur if the tracee provides bad
        // addresses as arguments to execve. In that cause, just continue on
        // as per usual and let the exec call fail.
        if (e.code().value() != EFAULT && e.code().value() != EIO) {
            throw e;
        }
    }

    int status;
    if (!resume() || !waitForStop(status)) {
        return;
    }
    if (!IS_EXEC_EVENT(status)) {
        /* Exec has failed!!! */
        if (!IS_SYSCALL_EVENT(status)) {
            throw BadTraceError(_pid,
                    "Expected a syscall-exit-stop after failed exec.");
        }
        _syscall = SYSCALL_NONE;

        // Get the return value to diagnose the cause of failure.
        size_t retval;
        if (!getSyscallRet(_pid, retval)) {
            expectEnded();
            return;
        }

        int err = (long)retval;
        if (err >= 0) {
            tlog << _pid << ": exec failed but returned " << err << endl;
        }

        _process->notifyExec(move(file), move(args), -err);
        return;
    }

    if (!resume() || !waitForStop(status)) {
        return;
    }
    if (!IS_SYSCALL_EVENT(status)) {
        throw BadTraceError(_pid, "Expected syscall-exit-stop after exec.");
    }
    _syscall = SYSCALL_NONE;

    _process->notifyExec(move(file), move(args), 0); // error code is 0
    _execed = true;
}

void Tracee::initiateWait(unique_ptr<BlockingCall> wait) {
    if (!wait->prepare(*this)) {
        expectEnded();
        return;
    }
    _blockingCall = move(wait);
}

/* Handle kill/tgkill/kill */
void Tracee::handleKill(pid_t killedId, int signal, bool toThread) {
    if (!resume()) {
        return;   
    }

    // we won't use the waitForStop helper function here, since we need a bit
    // more manual control to handle the case where the tracee SIGKILLs itself.
    int status;
    if (waitpid(_pid, &status, 0) == -1) {
        throw BadTraceError(_pid, errno, "waitpid");
    }
    if (!WIFSTOPPED(status)) {
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
            throw BadTraceError(_pid, "Tracee must have been SIGKILLed.");
        }
        if ((killedId == 0 || killedId == _pid || killedId == -_pid) 
                && signal == SIGKILL) {
            // The tracee SIGKILL'ed themselves or their own process group, so
            // it's still a valid kill() event even though we never reached a
            // syscall-exit-stop. (Technically it's possible for the SIGKILL to
            // have not originated from this kill call if another process sent
            // SIGKILL within the tiiiiny time window between the start of the
            // kill syscall and it actually killing the process, but this is
            // good enough for me I think). Unfortunately, PTRACE_GETSIGINFO is
            // of no use here, since it can't track SIGKILL'ed processes.
            Process::notifySentSignal(killedId, *_process.get(), 
                    _tracer.findNode(killedId).get(), signal, toThread);
        }
        handleWaitNotification(status);
        assert(_state == State::ENDED);
        return;
    }
    if (!IS_SYSCALL_EVENT(status)) {
        throw BadTraceError(_pid, "Expected syscall-exit-stop (handleKill).");
    }
    _state = State::STOPPED;
    _syscall = SYSCALL_NONE;

    size_t retval;
    if (!getSyscallRet(_pid, retval)) {
        expectEnded();
        return;
    }

    if (signal == 0 || retval != 0) {
        resume();
        return;
    }

    Process::notifySentSignal(killedId, *_process.get(),
            _tracer.findNode(killedId).get(), signal, toThread);
}

/* Handle a source location update from tracee using our fake syscall */
void Tracee::handleNewLocation(unsigned line, const char* function, 
        const char* file) 
{
    auto loc = make_unique<SourceLocation>();
    loc->line = line;
    if (!copyStringFromTracee(_pid, loc->func, function)) {
        expectEnded();
        return;
    }
    if (!copyStringFromTracee(_pid, loc->file, file)) {
        expectEnded();
        return;
    }
    _process->updateLocation(move(loc));
    resume(); // don't want this to interfere with anything
}

/* Call this on a syscall-entry-stop, with the syscall args provided and with
 * the syscall number stored in _syscall. The handlers called by this can
 * potentially re-resume the tracee and exit the syscall themselves. */
void Tracee::handleSyscallEntry(size_t args[SYS_ARG_MAX]) {
    assert(!_blockingCall);
    vdbg << _pid << " entering " << getSyscallName(_syscall) << endl;
    
    switch (_syscall) {
        case SYSCALL_PTRACE:
            // TODO make the ptrace call fail or do something more useful
            assert(!"Can't handle tracee calling ptrace yet");

        case SYSCALL_SETPGID:
            // TODO
            assert(!"Can't handle tracee calling setpgid yet");

        case SYSCALL_SETSID:
            // TODO
            assert(!"Can't handle tracee calling setsid yet");

        case SYSCALL_KILL:
            handleKill((pid_t)args[0], (int)args[1], false);
            break;

        case SYSCALL_TKILL:
            handleKill((pid_t)args[0], (int)args[1], true);
            break;

        case SYSCALL_TGKILL:
            handleKill((pid_t)args[1], (int)args[2], true);
            break;

        case SYSCALL_VFORK:
            // TODO make the vfork call fail or do something more useful
            assert(!"Can't handle tracee calling vfork yet");

        case SYSCALL_FORK:
            handleFork();
            break;

        case SYSCALL_EXECVE:
            handleExec((const char*)args[0], 
                    (const char**)args[1]);
            break;

        case SYSCALL_EXECVEAT:
            handleExec((const char*)args[1],
                    (const char**)args[2]);
            break;

        case SYSCALL_WAIT4:
            initiateWait(make_unique<Wait4Call>(
                (pid_t)args[0],
                (int*)args[1],
                (int)args[2]
            ));
            break;

        case SYSCALL_WAITID:
            initiateWait(make_unique<WaitIDCall>(
                (idtype_t)args[0],
                (id_t)args[1],
                (siginfo_t*)args[2],
                (int)args[3]
            ));
            break;

        case SYSCALL_CLONE:
            // TODO what if CLONE_THREAD or CLONE_PARENT is specified???
            // Then that is very much *unlike* a fork...?!?!?!
            if (IS_CLONE_LIKE_A_FORK(args)) {
                handleFork();
            } else {
                assert(!"I don't support threading at the moment...");
            }
            break;

        case SYSCALL_FAKE:
            handleNewLocation(
                (unsigned)args[0],
                (const char*)args[1],
                (const char*)args[2]
            );
            break;

        default:
            resume();
            break;
    }
}

/* Call this on a syscall-exit-stop. Setting _syscall back to SYSCALL_NONE is
 * left to the caller (please do that *after* calling this function). */
void Tracee::handleSyscallExit() {
    if (_blockingCall != nullptr) {
        // we just reached the syscall-exit-stop for a blocking system
        // call that we were trying to keep track of - so finish that.
        if (!_blockingCall->finalise(*this)) {
            dbg << _pid << " died on exit from blocking call" << endl;
            expectEnded();
            return;
        }
        dbg << _pid << " exited blocking call" << endl;
        _blockingCall.reset();
    }
    vdbg << _pid << " resuming after " << getSyscallName(_syscall) << endl;
    resume();
}

/* Should call this when the tracee is stopped (i.e., WIFSTOPPED==1) and we
 * want to verify that the tracee was stopped by the reception of a signal.
 * If not, an assertion will fail. Otherwise, the signal is dealt-with.
 * This function will leave the tracee in a stopped state (e.g., stopped()
 * == true). */
void Tracee::expectSignalStop(int status) {
    if (!WIFSTOPPED(status) 
            || IS_FORK_EVENT(status)
            || IS_EXIT_EVENT(status)
            || IS_CLONE_EVENT(status)
            || IS_SYSCALL_EVENT(status)
            || IS_EXEC_EVENT(status)) {
        throw BadTraceError(_pid, "Expected a signal-delivery-stop.");
    }
    assert(_state == State::STOPPED);
    int signal = WSTOPSIG(status);
    vdbg << _pid << " stopped by " << getSignalName(signal) << endl;

    siginfo_t info;
    if (ptrace(PTRACE_GETSIGINFO, _pid, 0, &info) == -1) {
        if (errno == ESRCH) {
            expectEnded();
            return;
        }
        throw BadTraceError(_pid, errno, "ptrace");
    }

    if (signal == _tracer.getHaltSignal()) {
        // It's the halt signal, but who sent it?
        if (info.si_pid == getpid()) {
            // It's a halt signal sent from the Tracer.
            _state = State::HALTED;
            dbg << _pid << " halted" << endl;
            return;
        }
    }

    // Otherwise it's just a normal signal.
    _process->notifySignaled(info.si_pid, signal);
    // Remember to deliver the signal when we're next resumed.
    _signal = signal;
}

/* Calls waitpid on the tracee and asserts that either WIFEXITED == 1 or
 * WIFSIGNALED == 1, then handles it via handleWaitNotification. */
void Tracee::expectEnded() {
    if (_state == State::ENDED) {
        return; // we've already got it
    }

    // TODO loop until we get an exit event (what if multiple events are queued
    // up? (Is that possible in these circumstances?)

    int status;
    if (waitpid(_pid, &status, 0) == -1) {
        throw BadTraceError(_pid, errno, "waitpid");
    }

    if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
        throw BadTraceError(_pid, "Expected tracee to have ended.");
    }
    handleWaitNotification(status);
}

/* Will wait until the tracee has stopped. If the tracee exits or is killed
 * before that happens, then expectEnded() is called to handle it and false
 * is returned. On other errors, an exception is thrown. This will filter
 * and handle halt signal events sent from the Tracer. */
bool Tracee::waitForStop(int& status) {
    assert(_state == State::RUNNING);

    if (waitpid(_pid, &status, 0) == -1) {
        throw BadTraceError(_pid, errno, "waitpid");
    }
    if (WIFSTOPPED(status)) {
        _state = State::STOPPED;
        return true;
    }

    handleWaitNotification(status);
    assert(_state == State::ENDED);
    return false;
}

/* If this tracee is stopped, then continue it. Throws a system_error if
 * something goes wrong. If the tracee dies while trying to resume it, then
 * that is handled via expectEnded and false is returned. If _signal is
 * currently non-zero, then that will be delivered to the tracee and then
 * set back to being zero. */
bool Tracee::resume() {
    if (_state == State::ENDED) {
        return false;
    }
    if (_state == State::RUNNING) {
        return true;
    }

    // tell ptracee to resume until it reaches a syscall-stop or other stop
    int err = ptrace(PTRACE_SYSCALL, _pid, 0, _signal);
    _signal = 0; // we've just delivered it, so reset

    if (err == -1) {
        if (errno == ESRCH) {
            expectEnded();
            return false;
        }
        throw BadTraceError(_pid, errno, "ptrace");
    }

    _state = State::RUNNING;
    return true;
}

/* Just so that we don't have to expose the 'bool' return value of resume() to
 * everyone else (the boolean return is for this class's internal use only). */
void Tracee::tryResume() {
    resume();
}

// TODO what kinds of events can occur between syscall entry and exit
void Tracee::handleWaitNotification(int status, bool halting) {
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        _process->notifyEnded(status);
        _state = State::ENDED;
        return;
    }

    if (!WIFSTOPPED(status)) {
        throw BadTraceError(_pid, "Tracee should be stopped.");
    }
    _state = State::STOPPED;

    if (IS_SYSCALL_EVENT(status)) {
        // This could be a syscall-entry-stop or a syscall-exit-stop - it's up
        // to us to keep track of which one it could be.
        if (_syscall == SYSCALL_NONE) {
            // Retrieve the syscall number and arguments first.
            size_t args[SYS_ARG_MAX];
            if (!whichSyscall(_pid, _syscall, args)) {
                expectEnded();
                return;
            }
            handleSyscallEntry(args);
        } else {
            handleSyscallExit();
            _syscall = SYSCALL_NONE;
        }
    } else if (IS_FORK_EVENT(status) 
            || IS_CLONE_EVENT(status) 
            || IS_EXEC_EVENT(status)) {
        // These events should only be generated when handling the fork, clone
        // and exec system calls. They should not be appearing now.
        throw BadTraceError(_pid, "Got fork/clone/exec event at weird time.");
    } else {
        expectSignalStop(status);
    }

    if (_state == State::HALTED && !halting) {
        // We've just ended up halted but the tracer is no longer trying to
        // halt us, so we just ignore the signal and continue until the next
        // event occurs.
        if (resume() && waitForStop(status)) {
            dbg << _pid << " ignored halt" << endl;
            handleWaitNotification(status);
        }
    }
}

Tracee::~Tracee() {
    if (!dead()) {
        kill(_pid, SIGKILL);
        while (waitpid(_pid, nullptr, 0) != -1) { }
    }
}

string_view Tracee::state() const {
    if (_blockingCall) {
        return _blockingCall->verb();
    }
    switch (_state) {
        case State::RUNNING:
            return "running";
        case State::STOPPED:
            return "stopped";
        case State::HALTED:
            return "halted";
        default:
            return _process->state();
    }
}

/* This is a static member function */
int Tracee::getTraceOptions() {
    return PTRACE_O_EXITKILL    // tracee gets SIGKILL'ed when we die
        | PTRACE_O_TRACESYSGOOD // prevent overloading of SIGTRAP
        | PTRACE_O_TRACEEXEC    // prevent overloading of SIGTRAP
        | PTRACE_O_TRACEFORK    // will also apply to fork-like clone()s
        | PTRACE_O_TRACECLONE;  // occurs when pthread_create is called
}

