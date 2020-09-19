#include <cassert>
#include <iostream>

#include "event.hpp"
#include "process.hpp"
#include "system.hpp"
#include "util.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::unique_ptr;
using std::shared_ptr;
using fmt::format;

string SourceLocation::to_string() const 
{
    return format("{}:{}:{}", file, func, line);
}

string ExecEvent::file() const 
{
    assert(!calls.empty());
    return calls.back().file;
}

bool ExecEvent::succeeded() const 
{
    assert(!calls.empty());
    return calls.back().errcode == 0;
}

void Event::print_tree(Indent indent) const 
{
    std::cerr << format("{}{}\n", indent, to_string());
}

string ForkEvent::to_string() const 
{
    return format("{} forked {}", owner.pid(), child->pid());
}

void ForkEvent::print_tree(Indent indent) const 
{
    Event::print_tree(indent);
    child->print_tree(indent + 1);
}

void ForkEvent::draw(IEventRenderer& renderer) const 
{
    renderer.draw_char(link_colour(), '+');
}

string get_wait_target_string(pid_t waitedId) 
{
    if (waitedId == -1)
    {
        return "any child";
    }
    else if (waitedId > 0)
    {
        return format("{}", waitedId);
    }
    else if (waitedId == 0)
    {
        return "their group";
    }
    else 
    {
        return format("{}", -waitedId);
    }
}

string WaitEvent::to_string() const 
{
    // Remember, WaitEvent's don't describe successful reap events, they only
    // describe waits that failed or haven't yet resulted in a reap.
    string target = get_wait_target_string(waitedId);
    if (nohang)
    {
        if (error == 0)
        {
            return format("{} waited for {} (WNOHANG) {{returned 0}}", 
                owner.pid(), target);
        }
        else
        {
            return format("{} waited for {} (WNOHANG) {{failed: {}}}",
                owner.pid(), target, strerror_s(error));
        }
    }
    else
    {
        if (error == 0)
        {
            return format("{} started waiting for {}", 
                owner.pid(), target);
        }
        else
        {
            return format("{} waited for {} {{failed: {}}}",
                owner.pid(), target, strerror_s(error));
        }
    }
}

void WaitEvent::draw(IEventRenderer& renderer) const 
{
    renderer.draw_char((error == 0) ? DEFAULT_COLOUR : BAD_WAIT_COLOUR, 'w');
}
    
ReapEvent::ReapEvent(Process& owner, 
                     unique_ptr<WaitEvent> wait, 
                     shared_ptr<Process> child)
    : LinkEvent(owner), child(std::move(child)), wait(std::move(wait)) 
{
    // Now that we are taking the place of the WaitEvent, we need to steal its
    // SourceLocation for ourselves. (use this-> to prevent shadowing).
    location = std::move(this->wait->location);
}

string ReapEvent::to_string() const 
{
    string target = get_wait_target_string(wait->waitedId);
    if (wait->nohang)
    {
        return format("{} reaped {} {{waited for {} (WNOHANG)}}", 
            owner.pid(), child->death_event().to_string(), target);
    }
    else
    {
        return format("{} reaped {} {{waited for {}}}",
            owner.pid(), child->death_event().to_string(), target);
    }
}

void ReapEvent::draw(IEventRenderer& renderer) const 
{
    char c;
    if (wait->waitedId == -1)
    {
        c = 'w';
    }
    else if (wait->waitedId > 0)
    {
        c = 'i';
    }
    else
    {
        c = 'g';
    }
    renderer.draw_char(link_colour(), c);
}

char ReapEvent::link_char() const 
{
    return child->killed() ? '~' : '-';
}

fmt::text_style ReapEvent::link_colour() const 
{
    return child->killed() ? KILLED_COLOUR : EXITED_COLOUR;
}

string RaiseEvent::to_string() const 
{
    if (killedId == -1)
    {
        return format("{} sent {} ({}) to everyone",
            owner.pid(), get_signal_name(signal), signal);
    }
    else if (killedId == 0)
    {
        return format("{} sent {} ({}) to their group",
            owner.pid(), get_signal_name(signal), signal);
    }
    else
    {
        string_view kind = toThread ? "thread" : "process";
        if (killedId == owner.pid())
        {
            return format("{} sent {} ({}) to themself {{as a {}}}",
                owner.pid(), get_signal_name(signal), signal, kind);
        }
        else
        {
            return format("{} sent {} ({}) to {} {{as a {}}}",
                owner.pid(), get_signal_name(signal), signal, killedId, kind);
        }
    }
}

void RaiseEvent::draw(IEventRenderer& renderer) const 
{
    //renderer.draw_char(SIGNAL_SEND_COLOUR, 'k');
    renderer.draw_string(SIGNAL_SEND_COLOUR, std::to_string(signal));
}
 
string KillEvent::to_string() const 
{
    pid_t dest = linked_path().pid();
    pid_t src = owner.pid();
    if (!sender)
    {
        std::swap(src, dest);
    }
    return format("{} sent {} ({}) to {} {{as a {}}}",
        src, get_signal_name(info->signal), info->signal, dest,
        info->toThread ? "thread" : "process");
}

void KillEvent::draw(IEventRenderer& renderer) const 
{
    //renderer.draw_char(SIGNAL_SEND_COLOUR, 'k');
    renderer.draw_string(SIGNAL_SEND_COLOUR, std::to_string(info->signal));
}

const Process& KillEvent::linked_path() const 
{
    return sender ? info->dest : info->source;
}

string SignalEvent::to_string() const 
{
    string_view action = killed ? "killed by" : "received";
    if (origin == -1)
    {
        return format("{} {} {} ({}) {{unknown sender}}",
            owner.pid(), action, get_signal_name(signal), signal);
    }
    else if (origin == 0 || origin == owner.pid())
    {
        return format("{} {} {} ({}) {{raised by self}}",
            owner.pid(), action, get_signal_name(signal), signal);
    }
    else if (origin == getpid())
    {
        return format("{} {} {} ({}) {{sent by tracer}}",
            owner.pid(), action, get_signal_name(signal), signal);
    }
    else
    {
        return format("{} {} {} ({}) {{sent by {}}}",
            owner.pid(), action, get_signal_name(signal), signal, origin);
    }
}

void SignalEvent::draw(IEventRenderer& renderer) const 
{
    if (!killed) 
    {
        renderer.draw_string(SIGNAL_COLOUR, std::to_string(signal));
        return;
    }

    if (owner.orphaned()) 
    {
        renderer.backtrack();
        renderer.draw_char(DEFAULT_COLOUR, '[');
    } 
    else if (!owner.reaped()) 
    {
        renderer.backtrack();
        renderer.draw_char(KILLED_COLOUR, '~');
    }

    renderer.draw_string(KILLED_COLOUR, std::to_string(signal));

    if (owner.orphaned()) 
    {
        renderer.draw_char(DEFAULT_COLOUR, ']');
    }
}

string ExitEvent::to_string() const 
{
    return format("{} exited {}", owner.pid(), status);
}

void ExitEvent::draw(IEventRenderer& renderer) const 
{
    if (owner.orphaned()) 
    {
        renderer.backtrack();
        renderer.draw_char(DEFAULT_COLOUR, '(');
    }

    renderer.draw_string(EXITED_COLOUR, std::to_string(status));

    if (owner.orphaned()) 
    {
        renderer.draw_char(DEFAULT_COLOUR, ')');
    }
}

string ExecCall::to_string(const ExecEvent& event) const 
{
    if (errcode == 0)
    {
        return format("{} execed {} [ {} ]", 
            event.owner.pid(), file, join(event.args));
    }
    else
    {
        if (file.empty())
        {
            return format("{} failed to exec: {}", 
                event.owner.pid(), strerror_s(errcode));
        }
        return format("{} failed to exec {}: {}",
            event.owner.pid(), file, strerror_s(errcode));
    }
}

string ExecEvent::to_string() const 
{
    assert(!calls.empty());
    if (calls.size() == 1)
    {
        return calls.back().to_string(*this);
    }
    return format("{} ({} attempts)", 
        calls.back().to_string(*this), calls.size());
}

void ExecEvent::print_tree(Indent indent) const 
{
    for (auto& call : calls) 
    {
        std::cerr << format("{}{}\n", indent, call.to_string(*this));
    }
}

void ExecEvent::draw(IEventRenderer& renderer) const 
{
    renderer.draw_char(succeeded() ? EXEC_COLOUR : BAD_EXEC_COLOUR, 'E');
}
