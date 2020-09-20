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

/* The configuration options for forktrace */
struct ForktraceOpts
{
    /* If false then we don't use a reaper process to learn about orphans and
     * we'll just ignore them. This is more of a debug option. Will cause the
     * list of orphaned tracees to pile up over time with no bound. Also see
     * the do_go() function in forktrace.cpp. */
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

/* Runs the specified command in forktrace. If the command is empty, or if the
 * user presses Ctrl+C while the command is running, then they will be taken to
 * the interactive command-line mode for forktrace. This is basically the entry
 * point for the program (after all of the parsing of command line options). 
 * Returns false on error (so the program should exit with an error status). */
bool forktrace(std::vector<std::string> command, ForktraceOpts opts);

#endif /* FORKTRACE_FORKTRACE_HPP */
