/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  diagram
 *
 *      TODO
 */
#ifndef FORKTRACE_DIAGRAM_HPP
#define FORKTRACE_DIAGRAM_HPP

#include <unordered_map>
#include <vector>
#include <memory>

#include "event.hpp"

class Window; // defined in terminal.hpp
class Process; // defined in process.hpp
class Drawer; // defined in diagram.cpp

/* This class allows you to build and draw diagrams produced by process trees.
 * This object does not take ownership (whether unique or shared) of each of
 * the Process/Event objects that constitute the process tree. Thus, while this
 * object exists, the process tree should not be modified or destroyed. */
class Diagram 
{
public:
    enum Options
    {
        SHOW_EXECS              = 1 << 0,
        SHOW_FAILED_EXECS       = 1 << 1,
        SHOW_NON_FATAL_SIGNALS  = 1 << 2,
        SHOW_SIGNAL_SENDS       = 1 << 3,
    };
    static constexpr int DEFAULT_OPTS = SHOW_EXECS | SHOW_SIGNAL_SENDS;

private:
    /* Represents the location of a Process as viewed on the diagram. */
    struct Path 
    {
        int startLine;
        int endLine; // -1 if not sure yet
        int lane; // -1 if not sure yet

        /* We use this to help sync up the paths of processes when we are
         * trying to link them up for a kill event. When a path has a pending
         * kill event, it should set this pointer equal to its partner in the
         * event so that the other path knows they're ready to dance ;-) */
        const Process* killPartner;

        Path(int startLine);
        Path() : startLine(-1), endLine(-1), lane(-1), killPartner(nullptr) { }
    };

    /* This class represents a point in a process's lifecycle which occurs on
     * a particular line of the diagram. Each line contains a bunch of lanes, 
     * which are occupied by these Nodes. */
    struct Node 
    {
        const Process& process;
        const Event* const event;
        const int next; // index of pending event, or -1 if none
        
        Node(const Process& process, const Event* event, int next)
            : process(process), event(event), next(next) { }

        bool zombie() const; // does this node correspond to a zombie process
        bool end_of_path() const; // are successors permitted after this?
        const Event* const next_event() const; // return null if no next event
        void print(Indent indent = 0) const;
    };

    /* The _renderer is a pointer just so that we don't have to define the
     * Drawer class in this header file (to avoid an incomplete type). */
    const Process& _leader; // the root of the process tree
    std::unique_ptr<Drawer> _renderer; // the object that renders the diagram
    size_t _laneCount;  // index of rightmost lane + 1
    int _options; // rendering config (TODO why no implicit int? compiler bug?)

    /* Stores the location/information about the path occupied by each process
     * on the diagram. I could store that information inside the Process class
     * but I'd rather not squeeze diagram information into process tree data
     * structures (and doing that has caused problems for me in the past). */
    std::unordered_map<const Process*, Path> _paths;

    /* The Nodes in this vector are organized so that their lane numbers are
     * in ascending order (however the index isn't necessarily equal to the
     * lane number, since there may be jumps).
     *
     * TODO make sure to be careful with having references to Nodes from this 
     * vector (they'll be invalidated if vector resized etc.). */
    std::vector<std::vector<Node>> _lines;

    /* Private functions, see source file. */
    int get_next_event(const Process& process, size_t start);
    Node get_successor(const Node& prevNode);
    Node continue_path(const Node& prevNode);
    Node start_path(const Process& process);
    void allocate_process_to_lane(std::vector<std::vector<Path*>>& lanes, 
            const Process& process);
    bool path_ready_to_end(const std::vector<Node>& prevLine,
            const Process& process) const;
    const Process* do_link_event(std::vector<Node>& curLine, int lineNum, 
            Path& path, const Node& prevNode, const LinkEvent& event);
    bool build_next_line();
    void draw_line(const std::vector<Node>& line, size_t lineNum);
    void draw();

public:
    /* This will build the diagram. Once this constructor returns, the diagram
     * will be accessible via the get_window() function. The `laneWidth` param
     * determines the number of columns used by each lane of the diagram. */
    Diagram(const Process& leader, size_t laneWidth, int opts = DEFAULT_OPTS);

    /* Needed to not make std::unique_ptr angry for some reason... */
    ~Diagram();

    /* Return the diagram drawn onto a curses pad. */
    const Window& result() const;

    /* Request the diagram to redraw (e.g. if tree is updated). */
    void redraw();

    /* Returns true if parts of the diagram had to be truncated to fit them on
     * the diagram (this means that the lane width should be increased). */
    bool truncated() const;

    /* Given the specified coordinates, try to find the event located there,
     * or return nullptr if we couldn't find it. The process of the path that
     * was found is stored in `process` (or nullptr if none was found). Even
     * if nullptr is returned, `process` may still be non-null. If a non-null
     * event is returned, then the index of that event in the process's event
     * list is stored in `eventIndex`. Otherwise, the index of the most recent
     * event occurring on that path (searching back from `line`) is stored (if
     * there are no events preceding `line`, then -1 is stored). */
    const Event* find(size_t lane,
                      size_t line,
                      const Process*& process,
                      int& eventIndex) const;

    /* Given the specified lane/line coordinates, convert them into the x/y
     * coordinates for the result() Window that we drew on to. If `lane` or
     * `line` are out of bounds, then the resulting coordinates will also be
     * out of bounds. */
    void get_coords(size_t lane, size_t line, size_t& x, size_t& y) const;

    /* Determines what lane the specified process appears in. An assertion will
     * fail if the process couldn't be found in any lane. */
    size_t locate(const Process& process) const;

    /* Get the number of lanes/lines on the diagram. */
    size_t line_count() const { return _lines.size(); }
    size_t lane_count() const { return _laneCount; }

    /* Get the leader process of this diagram. */
    const Process& leader() const { return _leader; }

    /* For debugging. Prints internal structures to log. */
    void print() const;
};

#endif /* FORKTRACE_DIAGRAM_HPP */
