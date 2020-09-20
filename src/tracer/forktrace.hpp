/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  forktrace
 *
 *      TODO
 */
#ifndef FORKTRACE_FORKTRACE_HPP
#define FORKTRACE_FORKTRACE_HPP

#include <vector>
#include <string>

class Process; // defined in process.hpp
class Tracer; // defined in tracer.hpp
class CommandParser; // defined in command.hpp

/* Just contains references to state needed by some other parts of the program.
 * Ceebs encapsulating this into a class - a struct will do. References mean I
 * don't have to include the full headers, which I hate doing... */
struct Forktrace
{
    struct Options
    {
        /* If false then we don't use a reaper process to learn about orphans 
         * and we'll just ignore them. This is more of a debug option. Will 
         * cause the list of orphaned tracees to pile up over time with no 
         * bound. Also see the do_go() function in forktrace.cpp. */
        bool reaper = true;

        /* Diagram options. */
        bool showNonFatalSignals = false;
        bool showExecs = true;
        bool showFailedExecs = false;
        bool showSignalSends = false;
        bool mergeExecs = true;
        size_t laneWidth = 4;

        /* Normally we only show the scroll-view in non-interactive mode if the
         * diagram can't fit, but this makes it always show. */
        bool forceScrollView = false;
    };

    Options& opts;
    Tracer& tracer;
    CommandParser& parser;
    std::vector<std::shared_ptr<Process>>& trees;

    Forktrace(Options& opts,
              Tracer& tracer, 
              CommandParser& parser, 
              decltype(trees) trees) 
        : opts(opts), tracer(tracer), parser(parser), trees(trees) { }
};

/* Runs the specified command in forktrace. If the command is empty, or if the
 * user presses Ctrl+C while the command is running, then they will be taken to
 * the interactive command-line mode for forktrace. This is basically the entry
 * point for the program (after all of the parsing of command line options). 
 * Returns false on error (so the program should exit with an error status). */
bool forktrace(std::vector<std::string> command, Forktrace::Options opts);

#endif /* FORKTRACE_FORKTRACE_HPP */
