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
    bool reaper = true; // reaper enabled?
    bool hideNonFatalSignals = true;
    bool hideExecs = false;
    bool hideFailedExecs = true;
    bool hideSignalSends = true;
    size_t laneWidth = 4;
};

/* Runs the specified command in forktrace. If the command is empty, or if the
 * user presses Ctrl+C while the command is running, then they will be taken to
 * the interactive command-line mode for forktrace. This is basically the entry
 * point for the program (after all of the parsing of command line options). 
 * Returns false on error (so the program should exit with an error status). */
bool forktrace(std::vector<std::string> command, ForktraceOpts settings);

#endif /* FORKTRACE_FORKTRACE_HPP */
