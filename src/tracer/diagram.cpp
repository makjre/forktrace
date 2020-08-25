#include <cassert>
#include <system_error>
#include <algorithm>

#include "terminal.h"
#include "diagram.h"
#include "process.h"
#include "event.h"
#include "util.h"

using namespace std;

/* The starting column for the diagram. Normally we want to give the diagram a
 * bit of breathing room on the left-hand side, so we'll allocate a free column
 * or two to make some extra space. We also need this for some events which can
 * draw backwards into the previous lane. */
constexpr auto LSHIFT = 1;

bool Diagram::Node::zombie() const {
    return process.reaped() && next == -1;
}

bool Diagram::Node::endOfPath() const {
    return !process.reaped() && next == -1;
}

const Event* const Diagram::Node::nextEvent() const {
    if (next == -1) {
        return nullptr;
    }
    return &process.getEvent(next);
}

void Diagram::Node::print(Indent indent) const {
    cout << indent << "my id: " << process.pid() << endl
        << indent << "events pending? = " << (nextEvent() != nullptr) << endl
        << indent << "endOfPath? = " << endOfPath() << endl;

    cout << indent << "my event: ";
    if (event) {
        cout << event->toString() << endl;
    } else {
        cout << "None" << endl;
    }

    cout << indent << "next event: ";
    if (nextEvent()) {
        cout << nextEvent()->toString() << endl;
    } else {
        cout << "None" << endl;
    }
}

Diagram::Path::Path(int startLine) 
    : startLine(startLine), endLine(-1), lane(-1), killPartner(nullptr) 
{
    assert(startLine >= 0);
}

void Drawer::start(size_t numLanes, size_t numLines) {
    _win = make_unique<Window>(numLanes * _laneWidth + LSHIFT, numLines * 2);
}

void Drawer::startLane(size_t lane) {
    _x = lane * _laneWidth + LSHIFT;
    if (_x < _xExtent) {
        _truncated = true;
    }
}

void Drawer::startLine(size_t line) {
    _xExtent = 0;
    _x = 0;
    _y = line * 2;
}

const Window& Drawer::result() const {
    assert(_win);
    return *_win.get();
}

/* Pads out the current position with link chars up to the lane width. Will
 * not affect the _xExtent of the diagram (thus, other events that draw on
 * top of this won't trigger the _truncated flag). */
void Drawer::drawLink(const LinkEvent& event) {
    Colour c = _win->setColour(event.linkColour());
    size_t laneStart = (_x - LSHIFT) / _laneWidth * _laneWidth + LSHIFT;
    size_t padding = laneStart + _laneWidth - _x;
    _win->drawChar(_x, _y, event.linkChar(), padding);
    _win->setColour(c);
    _x += padding;
}

/* Uses the specified colour and character to continue out the specified
 * lane up until the start of the next line. Only modifies lines in between
 * the main lines that contain the events. */
void Drawer::drawContinuation(size_t lane, Colour c, char ch) {
    c = _win->setColour(c);
    _win->drawChar(lane * _laneWidth + LSHIFT, _y + 1, ch);
    _win->setColour(c);
}

void Drawer::backtrack(size_t steps) {
    _x -= steps;
    if (_x < _xExtent) {
        _truncated = true;
    }
}

void Drawer::drawChar(Colour c, char ch, size_t count) {
    c = _win->setColour(c);
    _win->drawChar(_x, _y, ch, count);
    _win->setColour(c);
    _x = _xExtent = _x + count;
}

void Drawer::drawString(Colour c, string_view str) {
    c = _win->setColour(c);
    _win->drawString(_x, _y, str);
    _win->setColour(c);
    _x = _xExtent = _x + str.size();
}

/* Given the specified process, and a starting index to search from, find
 * the next event in the path (starting index included) which has not been
 * hidden from the diagram, according to _options. Returns -1 if no event
 * could be found (i.e., end of event list was reached). If the next event
 * in the path is a KillEvent, then this will update the killPartner field
 * for that path (see above). */
int Diagram::getNextEvent(const Process& process, int start) {
    for (int i = start; i < process.getEventCount(); ++i) {
        const Event& event = process.getEvent(i);

        if (_options.hideExecs) {
            if (dynamic_cast<const ExecEvent*>(&event)) {
                continue;
            }
        }
        if (_options.hideBadExecs) {
            auto exec = dynamic_cast<const ExecEvent*>(&event);
            if (exec && !exec->succeeded()) {
                continue;
            }
        }
        if (_options.hideNonFatalSignals) {
            auto signal = dynamic_cast<const SignalEvent*>(&event);
            if (signal && !signal->killed) {
                continue;
            }
        }
        if (_options.hideSignalSends) {
            if (dynamic_cast<const KillEvent*>(&event)) {
                continue;
            }
            if (dynamic_cast<const RaiseEvent*>(&event)) {
                continue;
            }
        }

        // Okay, we've found an event. If it's a KillEvent, then we'll signal
        // that in our path so that our partner knows about it.
        if (auto killEvent = dynamic_cast<const KillEvent*>(&event)) {
            auto path = _paths.find(&process);
            assert(path != _paths.end());
            assert(!path->second.killPartner);
            path->second.killPartner = &killEvent->linkedPath();
        }

        return i;
    }
    return -1;
}

/* Helper functions to create nodes. startPath returns the first node in
 * a path. continuePath returns a node that continues the path of prevNode
 * but otherwise does nothing. getSuccessor returns a node after prevNode
 * that consumes the next event in the path. */

Diagram::Node Diagram::getSuccessor(const Diagram::Node& prev) {
    const Process& process = prev.process;
    if (prev.next == -1) {
        return Node(process, nullptr, -1);
    }
    return Node(process, &process.getEvent(prev.next), 
            getNextEvent(process, prev.next + 1));
}

Diagram::Node Diagram::continuePath(const Diagram::Node& prev) {
    return Node(prev.process, nullptr, prev.next);
}

Diagram::Node Diagram::startPath(const Process& process) {
    return Node(process, nullptr, getNextEvent(process, 0));
}

/* Uses a recursive algorithm to allocate all of the processes in the tree
 * into a specific lane (and updates the `lane` field in each of the Path
 * objects). `lanes` should store the array of lanes so far, and `process`
 * should be the process currently being allocated. To start this function
 * off, just pass in a vector containing an empty lane, as well as the 
 * leader process. */
// TODO really sus to have pointers into a C++ container (it relies on the
// container not being modified during the lifetime of the pointers). I'll
// restructure this at some point I think.
void Diagram::allocateProcessToLane(vector<vector<Path*>>& lanes, 
        const Process& process)
{
    assert(_paths.find(&process) != _paths.end());
    assert(lanes.size() > 0);
    Path& myPath = _paths[&process]; 
    // Try to find what lane  we can plonk the path in. We iterate in reverse
    // order since we want children to 'fall on top' (so they appear to the
    // right). This method of lane allocation basically works like a game of
    // Tetris.
    bool collision = false;
    for (long i = lanes.size() - 1; i >= 0; --i) {
        // Check if `process` overlaps with anything in this lane
        for (const Path* path : lanes.at(i)) {
            if (myPath.endLine >= path->startLine 
                    && myPath.startLine <= path->endLine) {
                collision = true;
                break;
            }
        }
        if (collision) {
            if (i + 1 < lanes.size()) {
                // We've landed on something, so put it down in the above lane.
                myPath.lane = i + 1;
            } else {
                // We've landed on something, but there's no lane above us, so
                // we have to create a new lane for it.
                lanes.emplace_back();
                myPath.lane = lanes.size() - 1;
            }
            lanes.at(myPath.lane).push_back(&myPath);
            break;
        }
    }

    if (!collision) {
        // We didn't hit anything, which means we can fit it on the bottom
        myPath.lane = 0;
        lanes.at(0).push_back(&myPath);
    }

    // Now plonk down each of the children. We have to iterate backwards to
    // get the correct behaviour so that we can avoid overlapping lines.
    for (long i = process.getEventCount() - 1; i >= 0; --i) {
        auto forkEvent = dynamic_cast<const ForkEvent*>(&process.getEvent(i));
        if (forkEvent) {
            allocateProcessToLane(lanes, *forkEvent->child.get());
        }
    }
}

/* Checks if the path for this process is ready to terminate on this line
 * (based on what happened on the previous line). */
bool Diagram::pathReadyToEnd(const vector<Node>& prevLine, 
        const Process& process) const
{
    for (const Node& node : prevLine) {
        if (&node.process == &process) {
            return node.nextEvent() == nullptr;
        }
    }
    return false; // If it hasn't been created yet, then it isn't ready
}

/* Helper function for buildNextLine. Called when the next event along a
 * path is a LinkEvent (`prevNode` is the previous node in the path). This
 * function figures out the next node that should occur along the path and
 * adds it to the `curLine` vector. The return value is the process of the
 * path that the linking line for the LinkEvent ends on (a ForkEvent will
 * return null since the process of the child path does not exist in the
 * previous line). */
const Process* Diagram::doLinkEvent(vector<Node>& curLine, int lineNum, 
        Path& path, const Node& prevNode, const LinkEvent& event) 
{
    const vector<Node>& prevLine = _lines.back();
    const Process& other = event.linkedPath();

    if (dynamic_cast<const ForkEvent*>(&event)) {
        // This event will generate a new path
        assert(_paths.find(&other) == _paths.end());
        _paths[&other] = Path(lineNum);
        assert(_paths.find(&other) != _paths.end());
        curLine.push_back(getSuccessor(prevNode));
        curLine.push_back(startPath(other));
        return nullptr;
    }

    if (dynamic_cast<const ReapEvent*>(&event)) {
        // This event will remove an existing path from this line
        assert(_paths.find(&other) != _paths.end());
        if (!pathReadyToEnd(prevLine, other)) {
            curLine.push_back(continuePath(prevNode));
            return nullptr;
        }
        _paths[&other].endLine = lineNum;
        curLine.push_back(getSuccessor(prevNode));
        return &other;
    }

    if (auto killEvent = dynamic_cast<const KillEvent*>(&event)) {
        auto partner = _paths.find(&other);
        if (partner == _paths.end()) {
            // Our partner's path does not exist yet, so we wait
            curLine.push_back(continuePath(prevNode));
            return nullptr;
        }
        if (!path.killPartner) {
            // Our partner has already seen us and reset killPartner
            // back to null, which means we're both ready (they would
            // have to be to our left in the diagram).
            assert(!partner->second.killPartner);
            curLine.push_back(getSuccessor(prevNode));
            return nullptr;
        }
        if (partner->second.killPartner != &prevNode.process) {
            // The partner path isn't ready to connect with us yet
            curLine.push_back(continuePath(prevNode));
            return nullptr;
        }
        // Okay, both paths are ready to link up. We'll set both paths'
        // killPartners back to null, since neither are looking for a
        // connection any more at this point.
        partner->second.killPartner = path.killPartner = nullptr;
        curLine.push_back(getSuccessor(prevNode));
        return &other;
    }

    assert(!"Unreachable");
}

/* Generates the next line of the diagram. Uses all of the lines so far 
 * (in `_lines`) to help figure this out. Will return false if there are
 * no lines left to build, and appends to `_lines` otherwise. */
bool Diagram::buildNextLine() {
    assert(_lines.size() > 0);
    vector<Node> curLine;
    int lineNum = _lines.size();

    // There are horizontal lines drawn across the fork diagram between lanes
    // (for descendants of LinkEvent) to indicate forking/reaping/etc. We have
    // to keep track of if we are currently inside one of those lines so we do
    // not draw two of them on top of each other. If we are inside an event,
    // this points to the Process that it will terminate on. 
    const Process* eventEnd = nullptr;

    // Iterate over the previous line and check the next event that each of the
    // processes in the last line is waiting to perform. If a process from the
    // previous line wants to fork, then do that. If a process from the last
    // line wants to reap another process, then wait until that process is
    // finished, and then arrange for that to happen.
    for (const Node& prevNode : _lines.back()) {
        const Event* const event = prevNode.nextEvent(); 
        assert(_paths.find(&prevNode.process) != _paths.end());
        Path& path = _paths[&prevNode.process];

        // If the process has been orphaned or this is then parent and it has
        // nothing left to do, then we should end it on this line. We only want
        // to set the finishing line if it hasn't already finished (otherwise
        // it would never end and we'd be in an infinite loop).
        if (((&prevNode.process == &_leader && event == nullptr) 
                || prevNode.endOfPath()) && path.endLine == -1) {
            path.endLine = max(lineNum - 1, path.startLine);
        }

        // If we've reached the end of a dashed line, then indicate that.
        if (eventEnd == &prevNode.process) {
            eventEnd = nullptr;
        }

        // If this process has run out of events, then we'll only add it if
        // it's not ready to die (we could be waiting for it to be reaped).
        if (event == nullptr) {
            if (path.endLine == -1 || path.endLine >= lineNum) {
                curLine.push_back(continuePath(prevNode)); 
            }
            // Otherwise, let the process die
            continue;
        }

        if (auto linkEvent = dynamic_cast<const LinkEvent*>(event)) {
            if (eventEnd) {
                // We're currently embedded inside a connecting line between 
                // two lanes, so put off this event so that we don't have to 
                // deal with overlapping lines between lanes.
                curLine.push_back(continuePath(prevNode));
                continue;
            }
            eventEnd 
                = doLinkEvent(curLine, lineNum, path, prevNode, *linkEvent);
        } else {
            curLine.push_back(getSuccessor(prevNode));
        }
    }

    if (curLine.empty()) {
        return false;
    }
    _lines.push_back(move(curLine));
    return true;
}

/* Draw a single line of the diagram. `lineNum` is indexed from 0. */
void Diagram::drawLine(const vector<Node>& line, size_t lineNum) {
    _renderer.startLine(lineNum);

    // If we're currently in the middle of drawing a dashed line to another
    // lane for an event (e.g., forking or reaping), then we use this to keep
    // track of what that event currently is.
    const LinkEvent* curEvent = nullptr;
    // True if the current child event is 'reversed', which means it's going
    // backwards. In this case, we draw the termination on the left hand side
    // and the event on the right hand side.
    bool reversed = false;
    // The most recent lane index that we've looped through.
    size_t prevLane = 0;

    for (const Node& node : line) {
        auto pair = _paths.find(&node.process);
        assert(pair != _paths.end());
        Path path = pair->second;
        assert(path.lane >= prevLane);

        if (curEvent) {
            // Loop through the skipped lanes and draw the linking characters
            for (size_t i = prevLane + 1; i < path.lane; ++i) {
                _renderer.startLane(i);
                _renderer.drawLink(*curEvent);
            }
        }
        _renderer.startLane(path.lane);
        prevLane = path.lane;

        char pathChar = node.zombie() ? '.' : '|';
        Colour pathColour = Colour::WHITE;

        if (curEvent && (&curEvent->linkedPath() == &node.process)) {
            // We've reached the end of a dashed line across lanes.
            if (reversed) {
                curEvent->draw(_renderer);
            } else {
                _renderer.drawChar(Colour::WHITE, '+');
            }
            reversed = false;
            curEvent = nullptr;
        } else if (node.event) {
            if (auto linkEv = dynamic_cast<const LinkEvent*>(node.event)) {
                // This is the start of a dashed line across lanes.
                assert(!curEvent);
                curEvent = linkEv;
                // If this is a kill event, the 'dashed line' could possibly
                // be going backwards, in which case we'll draw it in reverse.
                if (auto killEv = dynamic_cast<const KillEvent*>(linkEv)) {
                    reversed = !killEv->sender;
                }
                if (reversed) {
                    _renderer.drawChar(Colour::WHITE, '+');
                } else {
                    curEvent->draw(_renderer);
                }
            } else {
                // Just a normal event.
                node.event->draw(_renderer);
            }
        } else {
            _renderer.drawChar(pathColour, pathChar);
        }

        if (curEvent) {
            _renderer.drawLink(*curEvent);
        }

        // Was this the last node in the path?
        if (path.endLine > lineNum) {
            _renderer.drawContinuation(path.lane, pathColour, pathChar);
        }
    }
}

/* Draw the entire diagram. Call this only after all of the lines have been
 * built and all of the lanes have been allocated. */
void Diagram::draw() {
    for (size_t i = 0; i < _lines.size(); ++i) {
        drawLine(_lines[i], i);
    }
}

Diagram::Diagram(const Process& leader, Options opts) 
    : _leader(leader), _options(opts), _renderer(opts.laneWidth)
{
    _paths[&leader] = Path(0);
    _lines.push_back({ startPath(leader) });

    // Figure out what nodes go in each line of the diagram
    while (buildNextLine()) { } 

    vector<vector<Path*>> lanes { vector<Path*>() };
    allocateProcessToLane(lanes, _leader);

    _renderer.start(lanes.size(), _lines.size());
    _laneCount = lanes.size();
    draw(); 
}

const Event* Diagram::find(size_t lane, size_t line, const Process*& process, 
        int& eventIndex) 
{
    if (line >= _lines.size()) {
        return nullptr;
    }

    for (const Node& node : _lines[line]) {
        auto it = _paths.find(&node.process);
        assert(it != _paths.end());
        if (it->second.lane == lane) {
            process = &node.process;

            if (node.next == -1) {
                // Will go to -1 if there are no events (this is desired)
                eventIndex = (int)node.process.getEventCount() - 1;
            } else {
                // Will go to -1 if node.next is event 0 (this is desired)
                eventIndex = node.next - 1;
            }

            return node.event;
        }
    }

    process = nullptr;
    return nullptr;
}

void Diagram::getCoords(size_t lane, size_t line, size_t& x, size_t& y) {
    x = lane * _renderer.laneWidth() + LSHIFT;
    y = line * 2;
}

size_t Diagram::locate(const Process& process) {
    auto it = _paths.find(&process);
    assert(it != _paths.end());
    return it->second.lane;
}

void Diagram::print() const {
    cout << "LINES" << endl;
    for (size_t i = 0; i < _lines.size(); ++i) {
        cout << Indent(1) << "Line " << i << endl;

        for (const Node& node : _lines.at(i)) {
            auto pair = _paths.find(&node.process);
            assert(pair != _paths.end());
            cout << Indent(2) << "Lane " << pair->second.lane << endl;
            node.print(3);
        }
    }

    cout << "PATHS" << endl;
    for (auto& pair : _paths) {
        cout << Indent(1) 
            << "pid=" << pair.first->pid()
            << " startLine=" << pair.second.startLine
            << " endLine=" << pair.second.endLine
            << " lane=" << pair.second.lane
            << endl; 
    }
}
