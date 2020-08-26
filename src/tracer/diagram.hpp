#ifndef DIAGRAM_H
#define DIAGRAM_H

#include <unordered_map>
#include <vector>
#include <memory>

#include "terminal.h"
#include "event.h"
#include "util.h"

class Process; // defined in process.h

/* An object that draws events onto the diagram. Only used in diagram.cpp (only
 * in header file cuz muh 'incomplete type'). */
class Drawer : public IEventRenderer {
private:
    size_t _laneWidth; // columns per each lane
    size_t _xExtent; // smallest index that can be drawn to without truncating
    size_t _x; // current x index (indexed from the left, from 0)
    size_t _y; // current y index (indexed from the top, from 0)
    bool _truncated; // set to true if lane width was too small
    std::unique_ptr<Window> _win; // What we're drawing to

public:
    Drawer(size_t laneWidth) : _laneWidth(laneWidth), _xExtent(0), _x(0), 
        _y(0), _truncated(false) { }

    void start(size_t numLanes, size_t numLines);
    void startLane(size_t lane);
    void startLine(size_t line);
    bool truncated() const { return _truncated; }
    size_t laneWidth() const { return _laneWidth; }
    const Window& result() const; // MUST call start first!!

    void drawLink(const LinkEvent& event);
    void drawContinuation(size_t lane, Colour c, char ch);
    
    /* Implementations for IEventRenderer */
    virtual void backtrack(size_t steps);
    virtual void drawChar(Colour c, char ch, size_t count = 1);
    virtual void drawString(Colour c, std::string_view str);
};

/* This class allows you to build and draw diagrams produced by process trees.
 * This object does not take ownership (whether unique or shared) of each of
 * the Process/Event objects that constitute the process tree. Thus, while this
 * object exists, the process tree should not be modified or destroyed. */
class Diagram {
public:
    struct Options {
        unsigned laneWidth;
        bool hideExecs; // if true, ALL execs are hidden, good or bad
        bool hideBadExecs; // if hideExecs==true, then this doesn't matter
        bool hideNonFatalSignals;
        bool hideSignalSends;

        Options(unsigned width, bool noExecs, bool noBadExecs,
                bool noReceives, bool noSends) 
            : laneWidth(width), hideExecs(noExecs), hideBadExecs(noBadExecs),
            hideNonFatalSignals(noReceives), hideSignalSends(noSends) { }
    };

private:
    /* Represents the location of a Process as viewed on the diagram. */
    struct Path {
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
    struct Node {
        const Process& process;
        const Event* const event;
        const int next; // index of pending event, or -1 if none
        
        Node(const Process& process, const Event* event, int next)
            : process(process), event(event), next(next) { }

        bool zombie() const; // does this node correspond to a zombie process
        bool endOfPath() const; // are successors permitted after this?
        const Event* const nextEvent() const; // return null if no next event
        void print(Indent indent = 0) const;
    };

    const Process& _leader; // the root of the process tree
    Drawer _renderer; // the object that renders the diagram
    size_t _laneCount;  // index of rightmost lane + 1
    Options _options; // rendering options

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

    int getNextEvent(const Process& process, int start);
    Node getSuccessor(const Node& prevNode);
    Node continuePath(const Node& prevNode);
    Node startPath(const Process& process);
    void allocateProcessToLane(std::vector<std::vector<Path*>>& lanes, 
            const Process& process);
    bool pathReadyToEnd(const std::vector<Node>& prevLine,
            const Process& process) const;
    const Process* doLinkEvent(std::vector<Node>& curLine, int lineNum, 
            Path& path, const Node& prevNode, const LinkEvent& event);
    bool buildNextLine();
    void drawLine(const std::vector<Node>& line, size_t lineNum);
    void draw();

public:
    /* This will build the diagram. Once this constructor returns, the diagram
     * will be accessible via the getWindow() function. The `laneWidth` param
     * determines the number of columns used by each lane of the diagram. */
    Diagram(const Process& leader, Options opts);

    /* Return the diagram drawn onto a curses pad. */
    const Window& result() { return _renderer.result(); }

    /* Returns true if parts of the diagram had to be truncated to fit them on
     * the diagram (this means that the lane width should be increased). */
    bool truncated() const { return _renderer.truncated(); }

    /* Given the specified coordinates, try to find the event located there,
     * or return nullptr if we couldn't find it. The process of the path that
     * was found is stored in `process` (or nullptr if none was found). Even
     * if nullptr is returned, `process` may still be non-null. If a non-null
     * event is returned, then the index of that event in the process's event
     * list is stored in `eventIndex`. Otherwise, the index of the most recent
     * event occurring on that path (searching back from `line`) is stored (if
     * there are no events preceding `line`, then -1 is stored). */
    const Event* find(size_t lane, size_t line, const Process*& process,
            int& eventIndex);

    /* Given the specified lane/line coordinates, convert them into the x/y
     * coordinates for the result() Window that we drew on to. If `lane` or
     * `line` are out of bounds, then the resulting coordinates will also be
     * out of bounds. */
    void getCoords(size_t lane, size_t line, size_t& x, size_t& y);

    /* Determines what lane the specified process appears in. An assertion will
     * fail if the process couldn't be found in any lane. */
    size_t locate(const Process& process);

    /* Get the number of lanes/lines on the diagram. */
    size_t lineCount() const { return _lines.size(); }
    size_t laneCount() const { return _laneCount; }

    /* For debugging. Prints internal structures to log. */
    void print() const;
};

#endif /* DIAGRAM_H */
