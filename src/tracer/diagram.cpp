/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  diagram
 *
 *      TODO
 */
#include <cassert>
#include <fmt/core.h>

#include "terminal.hpp"
#include "diagram.hpp"
#include "process.hpp"
#include "event.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::unique_ptr;
using fmt::format;

/* The starting column for the diagram. Normally we want to give the diagram a
 * bit of breathing room on the left-hand side, so we'll allocate a free column
 * or two to make some extra space. We also need this for some events which can
 * draw backwards into the previous lane. */
constexpr auto LSHIFT = 1;

class Drawer : public IEventRenderer 
{
private:
    size_t _laneWidth; // columns per each lane
    size_t _xExtent; // smallest index that can be drawn to without truncating
    size_t _x; // current x index (indexed from the left, from 0)
    size_t _y; // current y index (indexed from the top, from 0)
    bool _truncated; // set to true if lane width was too small
    unique_ptr<Window> _win; // What we're drawing to

public:
    Drawer(size_t lane_width) : _laneWidth(lane_width), _xExtent(0), _x(0), 
        _y(0), _truncated(false) { }

    void start(size_t numLanes, size_t numLines);
    void start_lane(size_t lane);
    void start_line(size_t line);
    bool truncated() const { return _truncated; }
    size_t lane_width() const { return _laneWidth; }
    const Window& result() const; // MUST call start first!!

    void draw_link(const LinkEvent& event);
    void draw_continuation(size_t lane, Colour c, char ch);
    
    /* Implementations for IEventRenderer */
    virtual void backtrack(size_t steps);
    virtual void draw_char(Colour c, char ch, size_t count = 1);
    virtual void draw_string(Colour c, std::string_view str);
};

bool Diagram::Node::zombie() const 
{
    return process.reaped() && next == -1;
}

bool Diagram::Node::end_of_path() const 
{
    return !process.reaped() && next == -1;
}

const Event* const Diagram::Node::next_event() const 
{
    if (next == -1) 
    {
        return nullptr;
    }
    return &process.event(next);
}

void Diagram::Node::print(Indent indent) const 
{
    std::cerr << format("{}my id: {}\n", indent, process.pid())
        << format("{}events pending? = {}\n", indent, next_event() != nullptr)
        << format("{}end of path? = {}\n", indent, end_of_path());

    if (event) 
    {
        std::cerr << format("{}my event: {}\n", indent, event->to_string());
    } 
    else 
    {
        std::cerr << format("{}my event: None\n", indent);
    }

    if (next_event()) 
    {
        std::cerr << format("{}next event: {}\n", 
            indent, next_event()->to_string());
    } 
    else 
    {
        std::cerr << format("{}next event: None\n", indent);
    }
}

Diagram::Path::Path(int startLine) 
    : startLine(startLine), endLine(-1), lane(-1), killPartner(nullptr) 
{
    assert(startLine >= 0);
}

void Drawer::start(size_t numLanes, size_t numLines) 
{
    size_t width = numLanes * _laneWidth + LSHIFT;
    size_t height = numLines * 2;
    _win = std::make_unique<Window>(width, height);
}

void Drawer::start_lane(size_t lane) 
{
    _x = lane * _laneWidth + LSHIFT;
    if (_x < _xExtent) 
    {
        _truncated = true;
    }
}

void Drawer::start_line(size_t line) 
{
    _xExtent = 0;
    _x = 0;
    _y = line * 2;
}

const Window& Drawer::result() const 
{
    assert(_win);
    return *_win.get();
}

/* Pads out the current position with link chars up to the lane width. Will
 * not affect the _xExtent of the diagram (thus, other events that draw on
 * top of this won't trigger the _truncated flag). */
void Drawer::draw_link(const LinkEvent& event) 
{
    Colour c = _win->set_colour(event.link_colour());
    size_t laneStart = (_x - LSHIFT) / _laneWidth * _laneWidth + LSHIFT;
    size_t padding = laneStart + _laneWidth - _x;
    _win->draw_char(_x, _y, event.link_char(), padding);
    _win->set_colour(c);
    _x += padding;
}

/* Uses the specified colour and character to continue out the specified
 * lane up until the start of the next line. Only modifies lines in between
 * the main lines that contain the events. */
void Drawer::draw_continuation(size_t lane, Colour c, char ch) 
{
    c = _win->set_colour(c);
    _win->draw_char(lane * _laneWidth + LSHIFT, _y + 1, ch);
    _win->set_colour(c);
}

void Drawer::backtrack(size_t steps) 
{
    _x -= steps;
    if (_x < _xExtent) 
    {
        _truncated = true;
    }
}

void Drawer::draw_char(Colour c, char ch, size_t count) 
{
    if (_x >= _win->width())
    {
        _truncated = true;
        return;
    }
    c = _win->set_colour(c);
    _win->draw_char(_x, _y, ch, count);
    _win->set_colour(c);
    _x = _xExtent = _x + count;
}

void Drawer::draw_string(Colour c, string_view str) 
{
    c = _win->set_colour(c);
    if (_x + str.size() > _win->width())
    {
        _truncated = true;
        str.remove_suffix(_x + str.size() - _win->width());
    }
    _win->draw_string(_x, _y, str);
    _win->set_colour(c);
    _x = _xExtent = _x + str.size();
}

/* Given the specified process, and a starting index to search from, find
 * the next event in the path (starting index included) which has not been
 * hidden from the diagram, according to _options. Returns -1 if no event
 * could be found (i.e., end of event list was reached). If the next event
 * in the path is a KillEvent, then this will update the killPartner field
 * for that path (see above). */
int Diagram::_get_next_event(const Process& process, size_t start) 
{
    for (size_t i = start; i < process.event_count(); ++i) 
    {
        const Event& event = process.event(i);

        if ((_options & SHOW_EXECS) == 0) 
        {
            if (dynamic_cast<const ExecEvent*>(&event)) 
            {
                continue;
            }
        }
        if ((_options & SHOW_FAILED_EXECS) == 0) 
        {
            auto exec = dynamic_cast<const ExecEvent*>(&event);
            if (exec && !exec->succeeded()) 
            {
                continue;
            }
        }
        if ((_options & SHOW_NON_FATAL_SIGNALS) == 0) 
        {
            auto signal = dynamic_cast<const SignalEvent*>(&event);
            if (signal && !signal->killed) 
            {
                continue;
            }
        }
        if ((_options & SHOW_SIGNAL_SENDS) == 0) 
        {
            if (dynamic_cast<const KillEvent*>(&event)) 
            {
                continue;
            }
            if (dynamic_cast<const RaiseEvent*>(&event)) 
            {
                continue;
            }
        }

        // Okay, we've found an event. If it's a KillEvent, then we'll signal
        // that it's in our path so that our partner knows about it.
        if (auto killEvent = dynamic_cast<const KillEvent*>(&event)) 
        {
            auto path = _paths.find(&process);
            assert(path != _paths.end());
            assert(!path->second.killPartner);
            path->second.killPartner = &killEvent->linked_path();
        }

        return i;
    }
    return -1;
}

/* Helper functions to create nodes. startPath returns the first node in
 * a path. _continue_path returns a node that continues the path of prevNode
 * but otherwise does nothing. getSuccessor returns a node after prevNode
 * that consumes the next event in the path. */

Diagram::Node Diagram::_get_successor(const Diagram::Node& prev) 
{
    const Process& process = prev.process;
    if (prev.next == -1) 
    {
        return Node(process, nullptr, -1);
    }
    return Node(process, &process.event(prev.next), 
        _get_next_event(process, prev.next + 1));
}

Diagram::Node Diagram::_continue_path(const Diagram::Node& prev) 
{
    return Node(prev.process, nullptr, prev.next);
}

Diagram::Node Diagram::_start_path(const Process& process) 
{
    return Node(process, nullptr, _get_next_event(process, 0));
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
void Diagram::_allocate_process_to_lane(vector<vector<Path*>>& lanes, 
                                       const Process& process)
{
    assert(_paths.find(&process) != _paths.end());
    assert(lanes.size() > 0);
    Path& myPath = _paths[&process]; 
    // Try to find what lane  we can plonk the path in. We iterate in reverse
    // order since we want children to 'fall on top' (so they appear to the
    // right). This method of lane allocation basically works like tetris.
    bool collision = false;
    for (long i = lanes.size() - 1; i >= 0; --i) 
    {
        // Check if `process` overlaps with anything in this lane
        for (const Path* path : lanes.at(i)) 
        {
            if (myPath.endLine >= path->startLine 
                && myPath.startLine <= path->endLine) 
            {
                collision = true;
                break;
            }
        }
        if (collision) 
        {
            if (size_t(i + 1) < lanes.size()) 
            {
                // We've landed on something, so put it down in the above lane.
                myPath.lane = i + 1;
            } 
            else 
            {
                // We've landed on something, but there's no lane above us, so
                // we have to create a new lane for it.
                lanes.emplace_back();
                myPath.lane = lanes.size() - 1;
            }
            lanes.at(myPath.lane).push_back(&myPath);
            break;
        }
    }

    if (!collision) 
    {
        // We didn't hit anything, which means we can fit it on the bottom
        myPath.lane = 0;
        lanes.at(0).push_back(&myPath);
    }

    // Now plonk down each of the children. We have to iterate backwards to
    // get the correct behaviour so that we can avoid overlapping lines.
    for (long i = process.event_count() - 1; i >= 0; --i) 
    {
        const Event* e = &process.event(i);
        if (auto forkEvent = dynamic_cast<const ForkEvent*>(e)) 
        {
            _allocate_process_to_lane(lanes, *forkEvent->child.get());
        }
    }
}

/* Checks if the path for this process is ready to terminate on this line
 * (based on what happened on the previous line). */
bool Diagram::_path_ready_to_end(const vector<Node>& prevLine, 
                                const Process& process) const
{
    for (const Node& node : prevLine) 
    {
        if (&node.process == &process) 
        {
            return node.next_event() == nullptr;
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
const Process* Diagram::_do_link_event(vector<Node>& curLine, 
                                      int lineNum, 
                                      Path& path, 
                                      const Node& prevNode, 
                                      const LinkEvent& event) 
{
    const vector<Node>& prevLine = _lines.back();
    const Process& other = event.linked_path();

    if (dynamic_cast<const ForkEvent*>(&event)) 
    {
        // This event will generate a new path
        assert(_paths.find(&other) == _paths.end());
        _paths[&other] = Path(lineNum);
        assert(_paths.find(&other) != _paths.end());
        curLine.push_back(_get_successor(prevNode));
        curLine.push_back(_start_path(other));
        return nullptr;
    }

    if (dynamic_cast<const ReapEvent*>(&event)) 
    {
        // This event will remove an existing path from this line
        assert(_paths.find(&other) != _paths.end());
        if (!_path_ready_to_end(prevLine, other)) 
        {
            curLine.push_back(_continue_path(prevNode));
            return nullptr;
        }
        _paths[&other].endLine = lineNum;
        curLine.push_back(_get_successor(prevNode));
        return &other;
    }

    if (dynamic_cast<const KillEvent*>(&event)) 
    {
        auto partner = _paths.find(&other);
        if (partner == _paths.end()) 
        {
            // Our partner's path does not exist yet, so we wait
            curLine.push_back(_continue_path(prevNode));
            return nullptr;
        }
        if (!path.killPartner) 
        {
            // Our partner has already seen us and reset killPartner
            // back to null, which means we're both ready (they would
            // have to be to our left in the diagram).
            assert(!partner->second.killPartner);
            curLine.push_back(_get_successor(prevNode));
            return nullptr;
        }
        if (partner->second.killPartner != &prevNode.process) 
        {
            // The partner path isn't ready to connect with us yet
            curLine.push_back(_continue_path(prevNode));
            return nullptr;
        }
        // Okay, both paths are ready to link up. We'll set both paths'
        // killPartners back to null, since neither are looking for a
        // connection any more at this point.
        partner->second.killPartner = path.killPartner = nullptr;
        curLine.push_back(_get_successor(prevNode));
        return &other;
    }

    assert(!"Unreachable");
}

/* Generates the next line of the diagram. Uses all of the lines so far 
 * (in `_lines`) to help figure this out. Will return false if there are
 * no lines left to build, and appends to `_lines` otherwise. */
bool Diagram::_build_next_line() 
{
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
    for (const Node& prevNode : _lines.back()) 
    {
        const Event* const event = prevNode.next_event(); 
        assert(_paths.find(&prevNode.process) != _paths.end());
        Path& path = _paths[&prevNode.process];

        // If the process has been orphaned or this is then parent and it has
        // nothing left to do, then we should end it on this line. We only want
        // to set the finishing line if it hasn't already finished (otherwise
        // it would never end and we'd be in an infinite loop).
        if (((&prevNode.process == &_leader && event == nullptr) 
            || prevNode.end_of_path()) && path.endLine == -1) 
        {
            path.endLine = std::max(lineNum - 1, path.startLine);
        }

        // If we've reached the end of a dashed line, then indicate that.
        if (eventEnd == &prevNode.process) 
        {
            eventEnd = nullptr;
        }

        // If this process has run out of events, then we'll only add it if
        // it's not ready to die (we could be waiting for it to be reaped).
        if (event == nullptr) 
        {
            if (path.endLine == -1 || path.endLine >= lineNum) 
            {
                curLine.push_back(_continue_path(prevNode)); 
            }
            continue; // Otherwise, let the process die
        }

        if (auto link = dynamic_cast<const LinkEvent*>(event)) 
        {
            if (eventEnd) 
            {
                // We're currently embedded inside a connecting line between 
                // two lanes, so put off this event so that we don't have to 
                // deal with overlapping lines between lanes.
                curLine.push_back(_continue_path(prevNode));
                continue;
            }
            eventEnd = _do_link_event(curLine, lineNum, path, prevNode, *link);
            continue;
        }
        curLine.push_back(_get_successor(prevNode));
    }

    if (curLine.empty()) 
    {
        return false;
    }
    _lines.push_back(move(curLine));
    return true;
}

/* Draw a single line of the diagram. `lineNum` is indexed from 0. */
void Diagram::_draw_line(const vector<Node>& line, size_t lineNum) 
{
    _renderer->start_line(lineNum);
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

    for (const Node& node : line) 
    {
        auto pair = _paths.find(&node.process);
        assert(pair != _paths.end());
        Path path = pair->second;
        assert(path.lane >= 0 && (size_t)path.lane >= prevLane);

        if (curEvent) 
        {
            // Loop through the skipped lanes and draw the linking characters
            for (size_t i = prevLane + 1; i < (size_t)path.lane; ++i) 
            {
                _renderer->start_lane(i);
                _renderer->draw_link(*curEvent);
            }
        }
        _renderer->start_lane(path.lane);
        prevLane = path.lane;

        char pathChar = node.zombie() ? '.' : '|';
        Colour pathColour = Colour::WHITE;

        if (curEvent && (&curEvent->linked_path() == &node.process)) 
        {
            // We've reached the end of a dashed line across lanes.
            if (reversed) 
            {
                curEvent->draw(*_renderer.get());
            } 
            else 
            {
                _renderer->draw_char(Colour::WHITE, '+');
            }
            reversed = false;
            curEvent = nullptr;
        } 
        else if (node.event) 
        {
            if (auto linkEv = dynamic_cast<const LinkEvent*>(node.event)) 
            {
                // This is the start of a dashed line across lanes.
                assert(!curEvent);
                curEvent = linkEv;
                // If this is a kill event, the 'dashed line' could possibly
                // be going backwards, in which case we'll draw it in reverse.
                if (auto killEv = dynamic_cast<const KillEvent*>(linkEv)) 
                {
                    reversed = !killEv->sender;
                }
                if (reversed) 
                {
                    _renderer->draw_char(Colour::WHITE, '+');
                } 
                else 
                {
                    curEvent->draw(*_renderer.get());
                }
            }
            else 
            {
                // Just a normal event.
                node.event->draw(*_renderer.get());
            }
        } 
        else 
        {
            _renderer->draw_char(pathColour, pathChar); // continue the path
        }

        if (curEvent) 
        {
            _renderer->draw_link(*curEvent); // TODO needed?
        }

        // Was this the last node in the path?
        assert(path.endLine >= 0);
        if ((size_t)path.endLine > lineNum) 
        {
            _renderer->draw_continuation(path.lane, pathColour, pathChar);
        }
    }
}

/* Draw the entire diagram. Call this only after all of the lines have been
 * built and all of the lanes have been allocated. */
void Diagram::_draw() 
{
    for (size_t i = 0; i < _lines.size(); ++i) 
    {
        _draw_line(_lines.at(i), i);
    }
}

Diagram::Diagram(const Process& leader, size_t laneWidth, int opts) 
    : _leader(leader), _options(opts)
{
    _renderer = std::make_unique<Drawer>(laneWidth);
    redraw();
}

Diagram::~Diagram()
{
    // Needed here (specifically here) to stop unique_ptr from getting angry
}

const Window& Diagram::result() const
{
    return _renderer->result();
}

void Diagram::redraw()
{
    _paths.clear();
    _lines.clear();
    _paths[&_leader] = Path(0);
    _lines.push_back({ _start_path(_leader) });
    
    // Figure out what nodes go in each line of the diagram
    while (_build_next_line()) { } 

    // Recursively allocates all the paths to a lane
    vector<vector<Path*>> lanes { vector<Path*>() };
    _allocate_process_to_lane(lanes, _leader);

    _renderer->start(lanes.size(), _lines.size());
    _laneCount = lanes.size();
    _draw(); 
}

bool Diagram::truncated() const
{
    return _renderer->truncated();
}

// TODO refactor so that it returns process instead and stores event?
const Event* Diagram::find(size_t lane, 
                           size_t line, 
                           const Process*& process, 
                           int& eventIndex) const
{
    if (line >= _lines.size()) 
    {
        return nullptr;
    }
    for (const Node& node : _lines.at(line)) 
    {
        auto it = _paths.find(&node.process);
        assert(it != _paths.end());
        assert(it->second.lane >= 0);
        if ((size_t)it->second.lane == lane) 
        {
            if (node.next == -1) 
            {
                // Will go to -1 if there are no events (this is desired)
                eventIndex = (int)node.process.event_count() - 1;
            } 
            else 
            {
                // Will go to -1 if node.next is event 0 (this is desired)
                eventIndex = node.next - 1;
            }
            process = &node.process;
            return node.event;
        }
    }
    process = nullptr;
    return nullptr;
}

void Diagram::get_coords(size_t lane, size_t line, size_t& x, size_t& y) const
{
    x = lane * _renderer->lane_width() + LSHIFT;
    y = line * 2;
}

size_t Diagram::locate(const Process& process) const
{
    auto it = _paths.find(&process);
    assert(it != _paths.end());
    return it->second.lane;
}

void Diagram::print() const 
{
    std::cerr << "LINES\n";
    for (size_t i = 0; i < _lines.size(); ++i) 
    {
        std::cerr << format("{}Line {}\n", Indent(1), i);
        for (const Node& node : _lines.at(i)) 
        {
            auto pair = _paths.find(&node.process);
            assert(pair != _paths.end());
            std::cerr << format("{}Lane {}\n", Indent(2), pair->second.lane);
            node.print(3);
        }
    }
    std::cerr << "PATHS\n";
    for (auto& [process, path] : _paths) 
    {
        std::cerr << format("{}pid={} startLine={} endLine={} lane={}\n",
            Indent(1), process->pid(), path.startLine, path.endLine, path.lane);
    }
}
