#include <cassert>
#include <sstream>
#include <cstring>

#include "event.h"
#include "process.h"
#include "system.h"
#include "terminal.h"
#include "util.h"

using namespace std;

string SourceLocation::toString() const {
    ostringstream oss;
    oss << file << ':' << func << ':' << line;
    return oss.str();
}

string ExecEvent::file() const {
    assert(!calls.empty());
    return calls.back().file;
}

bool ExecEvent::succeeded() const {
    assert(!calls.empty());
    return calls.back().errcode == 0;
}

void Event::setLocation(unique_ptr<SourceLocation> location) {
    loc = move(location);
}

void Event::printTree(Indent indent) const {
    cout << indent << toString() << endl;
}

string ForkEvent::toString() const {
    ostringstream oss;
    oss << owner.pid() << " forked " << child->pid();
    return oss.str();
}

void ForkEvent::printTree(Indent indent) const {
    Event::printTree(indent);
    child->printTree(indent + 1);
}

void ForkEvent::draw(IEventRenderer& renderer) const {
    renderer.drawChar(linkColour(), '+');
}

string getWaitTargetString(pid_t waitedId) {
    ostringstream oss;
    if (waitedId == -1) {
        oss << "any child";
    } else if (waitedId > 0) {
        oss << waitedId;
    } else if (waitedId == 0) {
        oss << "their group";
    } else {
        oss << "group " << -waitedId;
    }
    return oss.str();
}

string WaitEvent::toString() const {
    ostringstream oss;
    oss << owner.pid();
    if (nohang) {
        oss << " waited for " << getWaitTargetString(waitedId) 
            << " (WNOHANG) ";
        if (error == 0) {
            oss << "{returned 0}";
        } else {
            oss << "{failed: " << errStr(error) << '}';
        }
    } else {
        if (error == 0) {
            oss << " started waiting for " << getWaitTargetString(waitedId);
        } else {
            oss << " waited for " << getWaitTargetString(waitedId)
                << " {failed: " << errStr(error) << '}';
        }
    }
    return oss.str();
}

void WaitEvent::draw(IEventRenderer& renderer) const {
    renderer.drawChar((error == 0) ? Colour::WHITE : BAD_WAIT_COLOUR, 'w');
}
    
ReapEvent::ReapEvent(Process& owner, unique_ptr<WaitEvent> wait, 
        shared_ptr<Process> child)
    : LinkEvent(owner), child(move(child)), wait(move(wait)) 
{
    // Now that we are taking the place of the WaitEvent, we need to steal its
    // SourceLocation for ourselves. (use this-> to prevent shadowing).
    loc = move(this->wait->loc); 
}

string ReapEvent::toString() const {
    ostringstream oss;
    oss << owner.pid() << " reaped " << child->deathEvent().toString(); 
    oss << " {waited for " << getWaitTargetString(wait->waitedId);
    if (wait->nohang) {
        oss << " (WNOHANG)}";
    } else {
        oss << '}';
    }
    return oss.str();
}

void ReapEvent::draw(IEventRenderer& renderer) const {
    Colour c = linkColour();
    if (wait->waitedId == -1) {
        renderer.drawChar(c, 'w');
    } else if (wait->waitedId > 0) {
        renderer.drawChar(c, 'i');
    } else {
        renderer.drawChar(c, 'g');
    }
}

char ReapEvent::linkChar() const {
    return child->killed() ? '~' : '-';
}

Colour ReapEvent::linkColour() const {
    return child->killed() ? KILLED_COLOUR : EXITED_COLOUR;
}

string RaiseEvent::toString() const {
    ostringstream oss;
    oss << owner.pid() << " sent " << getSignalName(signal) << '('
        << signal << ") to ";

    if (killedId == -1) {
        oss << "everyone";
    } else if (killedId == 0) {
        oss << "their group";
    } else if (killedId < 0) {
        oss << "group " << -killedId;
    } else {
        if (killedId == owner.pid()) {
            oss << "themself {as a ";
        } else {
            oss << killedId << " {as a ";
        }
        oss << (toThread ? "thread}" : "process}");
    }

    return oss.str();
}

void RaiseEvent::draw(IEventRenderer& renderer) const {
    //renderer.drawChar(SIGNAL_SEND_COLOUR, 'k');
    renderer.drawString(SIGNAL_SEND_COLOUR, to_string(signal));
}
 
string KillEvent::toString() const {
    ostringstream oss;
    if (sender) {
        oss << owner.pid() << " sent " << getSignalName(info->signal) 
            << '(' << info->signal << ')' << " to " << linkedPath().pid();
    } else {
        oss << linkedPath().pid() << " sent " << getSignalName(info->signal) 
            << '(' << info->signal << ')' << " to " << owner.pid();
    }
    oss << " {" << (info->toThread ? "as a thread" : "as a process") << '}';
    return oss.str();
}

void KillEvent::draw(IEventRenderer& renderer) const {
    //renderer.drawChar(SIGNAL_SEND_COLOUR, 'k');
    renderer.drawString(SIGNAL_SEND_COLOUR, to_string(info->signal));
}

const Process& KillEvent::linkedPath() const {
    return sender ? info->dest : info->source;
}

string SignalEvent::toString() const {
    ostringstream oss;
    oss << owner.pid() << (killed ? " killed by " : " received ") 
        << getSignalName(signal) << '(' << signal << ')';
    if (origin == -1) {
        oss << " {unknown sender}";
    } else if (origin == 0 || origin == owner.pid()) {
        oss << " {raised by self}";
    } else if (origin == getpid()) {
        oss << " {sent by tracer}";
    } else {
        oss << " {sent by " << origin << '}';
    }
    return oss.str();
}

void SignalEvent::draw(IEventRenderer& renderer) const {
    if (!killed) {
        renderer.drawString(SIGNAL_COLOUR, to_string(signal));
        return;
    }

    if (owner.orphaned()) {
        renderer.backtrack();
        renderer.drawChar(Colour::WHITE, '[');
    } else if (!owner.reaped()) {
        renderer.backtrack();
        renderer.drawChar(KILLED_COLOUR, '~');
    }

    renderer.drawString(KILLED_COLOUR, to_string(signal));

    if (owner.orphaned()) {
        renderer.drawChar(Colour::WHITE, ']');
    }
}

string ExitEvent::toString() const {
    ostringstream oss;
    oss << owner.pid() << " exited " << status;
    return oss.str();
}

void ExitEvent::draw(IEventRenderer& renderer) const {
    if (owner.orphaned()) {
        renderer.backtrack();
        renderer.drawChar(Colour::WHITE, '(');
    }

    renderer.drawString(EXITED_COLOUR, to_string(status));

    if (owner.orphaned()) {
        renderer.drawChar(Colour::WHITE, ')');
    }
}

string ExecCall::toString(const ExecEvent& event) const {
    ostringstream oss;
    if (errcode == 0) {
        oss << event.owner.pid() << " execed " << file << " [ ";
        for (auto& arg : event.args) {
            oss << arg << ' ';
        }
        oss << ']';
    } else {
        oss << event.owner.pid() << " failed to exec";
        if (!file.empty()) {
            oss << ' ' << file;   
        }
        oss << ": " << errStr(errcode);
    }
    return oss.str();
}

string ExecEvent::toString() const {
    assert(!calls.empty());
    ostringstream oss;
    oss << calls.back().toString(*this);
    if (calls.size() > 1) {
        oss << " (" << calls.size() << " attempts)";
    }
    return oss.str();
}

void ExecEvent::printTree(Indent indent) const {
    for (auto& call : calls) {
        cout << indent << call.toString(*this) << endl;
    }
}

void ExecEvent::draw(IEventRenderer& renderer) const {
    renderer.drawChar(succeeded() ? EXEC_COLOUR : BAD_EXEC_COLOUR, 'E');
}
