#ifndef EVENT_H
#define EVENT_H

#include <memory>
#include <string>
#include <vector>
#include <cassert>

#include "terminal.hpp"
#include "util.hpp"

class Process; // defined in process.h
struct ExecEvent; // defined in this file

constexpr auto EXITED_COLOUR = Colour::GREEN_BOLD;
constexpr auto KILLED_COLOUR = Colour::RED_BOLD;
constexpr auto SIGNAL_COLOUR = Colour::YELLOW;
constexpr auto EXEC_COLOUR = Colour::BLUE_BOLD;
constexpr auto BAD_EXEC_COLOUR = Colour::RED;
constexpr auto BAD_WAIT_COLOUR = Colour::RED;
constexpr auto SIGNAL_SEND_COLOUR = Colour::MAGENTA;

/* An interface that Event objects need to draw themselves. The renderer draws
 * the diagram line by line. As the renderer draws a line (from left to right)
 * it will ask events to draw themselves when it hits the position in the line
 * where the event is supposed to appear. The event renderer gives each event
 * a set of functions it can use to draw itself via this interface. */
class IEventRenderer {
public:
    /* Moves the cursor back (to the left) by the specified number of steps. */
    virtual void backtrack(size_t steps = 1) = 0;

    /* Draws a single character with the current colour. If the second param
     * is provided, then the character is repeated the specified number of
     * times. */
    virtual void drawChar(Colour c, char ch, size_t count = 1) = 0;

    /* Draws a string with the current colour. */
    virtual void drawString(Colour c, std::string_view str) = 0;
};

struct SourceLocation {
    std::string file;
    std::string func;
    unsigned line;

    std::string toString() const;
};

struct Event {
    Process& owner;
    std::unique_ptr<SourceLocation> loc; // TODO

    Event(Process& owner) : owner(owner) { }
    virtual ~Event() { }
    // TODO do i need to declare descendant functions virtual?

    void setLocation(std::unique_ptr<SourceLocation> location);

    virtual std::string toString() const = 0;
    virtual void printTree(Indent indent = 0) const;
    virtual void draw(IEventRenderer& renderer) const = 0; 
};

/* An event that causes a horizontal line to be drawn, connecting the event
 * to another path. This horizontal line could represent a branch in the fork
 * diagram, or it could represent reaping, or maybe something else? */
struct LinkEvent : Event {
    LinkEvent(Process& owner) : Event(owner) { }

    virtual const Process& linkedPath() const = 0;
    virtual char linkChar() const = 0;
    virtual Colour linkColour() const { return Colour::WHITE; };
};

/* An event that generates a child who sends SIGCHLD to the parent */
struct ForkEvent : LinkEvent {
    std::shared_ptr<Process> child;

    ForkEvent(Process& owner, std::shared_ptr<Process> child) 
        : LinkEvent(owner), child(std::move(child)) { }

    virtual std::string toString() const;
    virtual void printTree(Indent indent) const;
    virtual void draw(IEventRenderer& renderer) const;
    virtual const Process& linkedPath() const { return *child.get(); }
    virtual char linkChar() const { return '-'; }
};

/* Represents a wait call that hasn't yet resulted in a child getting reaped
 * (either due to not finishing yet or due to it failing). If the wait call 
 * completes and results in a reap, then this event will get removed from the 
 * process's event list and get put inside a ReapEvent instead. */
struct WaitEvent : Event {
    pid_t waitedId;

    /* This is a bit confusing, but I'd rather not add extra variables since I
     * don't need them. If error==0 and nohang==true, then we have two possible
     * cases:
     *
     *  (1) The wait call returned 0 since no children were ready.
     *  (2) The wait call succeeded and a child was reaped. 
     *
     * This struct doesn't actually distinguish at all between these two cases.
     * The only way the process tree is able to distinguish between them is due
     * to this WaitEvent getting taken off the Process's events list and being
     * stuffed into a ReapEvent if it actually results in a reap. */
    int error;
    bool nohang;

    /* Initiate a wait that hasn't returned yet. If you find out that the wait
     * failed, you just set ->error to the error status and that's all. */
    WaitEvent(Process& owner, pid_t waitedId, bool nohang)
        : Event(owner), waitedId(waitedId), error(0), nohang(nohang) { }

    virtual std::string toString() const;
    virtual void draw(IEventRenderer& renderer) const;
};

/* A process is reaped by an ancestor via wait4 or waitid. */
struct ReapEvent : LinkEvent {
    std::shared_ptr<Process> child;
    std::unique_ptr<WaitEvent> wait; // the WaitEvent that triggered this

    ReapEvent(Process& owner, std::unique_ptr<WaitEvent> wait, 
            std::shared_ptr<Process> child);

    virtual std::string toString() const;
    virtual void draw(IEventRenderer& renderer) const;
    virtual const Process& linkedPath() const { return *child.get(); }
    virtual char linkChar() const;
    virtual Colour linkColour() const;
};

/* A process sends a signal to either itself, a process group, or it sends it
 * to another process who we couldn't find in the process tree at the time (via
 * kill, tkill or tgkill). */
struct RaiseEvent : Event {
    pid_t killedId; // same meaning as PID argument of kill(2)
    int signal;
    bool toThread; // Was this signal targetted at this specific thread?

    RaiseEvent(Process& owner, pid_t dest, int signal, bool toThread)
        : Event(owner), killedId(dest), signal(signal), toThread(toThread) { }

    virtual std::string toString() const;
    virtual void draw(IEventRenderer& renderer) const;
};

/* The shared information held by the source and destination processes of a 
 * KillEvent (see the definition below). */
struct KillInfo {
    const Process& source;
    const Process& dest;
    int signal;
    bool toThread;

    KillInfo(Process& source, Process& dest, int signal, bool toThread)
        : source(source), dest(dest), signal(signal), toThread(toThread) { }
};

/* This event describes when process A sends a signal to a (different) process
 * B via kill/tkill/tgkill. These events should be added in pairs --- one to
 * the receiving process (with sender=false) and one to the sending process
 * (with sender=true). Both KillEvents out of the pair both reference the same
 * shared KillInfo. */
struct KillEvent : LinkEvent {
    std::shared_ptr<KillInfo> info; // both the sender and receiver need this
    bool sender; // are we the sender, or the receiver?

    KillEvent(Process& owner, std::shared_ptr<KillInfo> info, bool sender)
        : LinkEvent(owner), info(std::move(info)), sender(sender) { }

    virtual std::string toString() const;
    virtual void draw(IEventRenderer& renderer) const;
    virtual const Process& linkedPath() const;

    /* We don't know whether the sender will be to the left of the receiver on
     * the diagram, or vice versa. The diagram renderer will figure out which
     * of the two will go on the left-hand side, and will draw the linking line
     * as it moves left-to-right. If it encounters the sender first, then we
     * want it drawing ">>>>" as it goes left-to-right towards the receiver.
     * On the other hand, if it encounters the receiver first, then we want it
     * drawing "<<<<" as it goes left-to-right towards the sender. */
    virtual char linkChar() const { return sender ? '>' : '<'; }
};

/* A process receives a signal, which may or may not kill it. */
struct SignalEvent : Event {
    pid_t origin; // -1 means don't know, 0 or own pid means self
    int signal;
    bool killed;

    SignalEvent(Process& owner, pid_t origin, int sig, bool killed) 
        : Event(owner), origin(origin), signal(sig), killed(killed) { }

    SignalEvent(Process& owner, int sig, bool killed)
        : SignalEvent(owner, -1, sig, killed) { }

    virtual std::string toString() const;
    virtual void draw(IEventRenderer& renderer) const;
};

/* A process exits, causing it to terminate. */
struct ExitEvent : Event {
    int status;

    ExitEvent(Process& owner, int status) : Event(owner), status(status) { }
    virtual std::string toString() const;
    virtual void draw(IEventRenderer& renderer) const;
};

/* Describes the state of a successful or failed exec call. */
struct ExecCall {
    std::string file;
    int errcode; // an errno value

    ExecCall(std::string file, int err) 
        : file(std::move(file)) , errcode(err) { }

    std::string toString(const ExecEvent& owner) const;
};

/* Describes a call to exec. This struct allows us to group together strings
 * of failed exec calls into one event. This is desirable because C functions
 * like execvp or execlp will search the system $PATH variable, but this is
 * internally implemented just by trying to execve on each directory inside
 * $PATH - so we can hide all of the failed attempts using this struct and
 * only show the successful one on the diagram. */
struct ExecEvent : Event {
    std::vector<ExecCall> calls;
    std::vector<std::string> args;

    /* This constructor initialises the event with the first exec call that has
     * been made for this event. */
    ExecEvent(Process& owner, std::string path, 
            std::vector<std::string> args, int err)
        : Event(owner), calls{ExecCall(std::move(path), err)}, 
        args(std::move(args)) { }

    virtual std::string toString() const;
    virtual void printTree(Indent indent) const;
    virtual void draw(IEventRenderer& renderer) const;

    /* Gets the most recent file that was execed. If there are no calls in the
     * list than an assertion will fail. */
    std::string file() const;

    /* Return true if this exec succeeded. An assertion fails if there were no
     * exec calls in the list. */
    bool succeeded() const;
};

#endif /* EVENT_H */
