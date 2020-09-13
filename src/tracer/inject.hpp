#ifndef FORKTRACE_INJECT_HPP
#define FORKTRACE_INJECT_HPP

#include <vector>
#include <string>

/* Performs the injection action for forktrace. It runs the provided compiler
 * command (argv) but injects the forktrace.h file into each of the files from
 * `files`. It does this without modifying the files on disk. If the process
 * fails, then error messages will be printed and false will be returned. */
bool do_inject(std::vector<std::string> files, std::vector<std::string> argv);

#endif /* FORKTRACE_INJECT_HPP */
