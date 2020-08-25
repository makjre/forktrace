#ifndef UTIL_H
#define UTIL_H

#include <string>
#include <iostream>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* dbg is for debugging output, vdbg is for 'verbose output' that, when it is
 * enabled, will typically produce lots and lots of output (e.g., it logging
 * that would occur for every single syscall-entry-stop for a tracee would be
 * considered 'verbose'). The logging stream is just for normal output. */
extern std::ostream tlog;
extern std::ostream dbg;
extern std::ostream vdbg;

/* Set the debugging log level. If level==0, then both of the debug streams are
 * disabled. If level==1, then dbg is enabled. If level==2, then both streams
 * are enabled. If level is >2, then a runtime_error is thrown. */
void setDebugLogLevel(unsigned level);

/* Enable or disable all logging to the `tlog`, `dbg` or `vdbg` streams. */
void setLogEnabled(bool enabled);

/* Strips all leading and trailing whitespace from the specified string. */
void strip(std::string& str);

/* An adapter that reverses a container for use with range-based for loops.
 * Basically an attempt to emulate python's reversed() with C++ templates! */
template <class Container>
class ReverseIterator {
private:
    Container& container;

public:
    ReverseIterator(Container& container) : container(container) { }
    auto begin() { return container.rbegin(); }
    auto end() { return container.rend(); }
};

template <class Container>
auto reverse(Container& container) {
    return ReverseIterator<Container>(container);
}

template <class Container>
auto reverse(const Container& container) {
    return ReverseIterator<const Container>(container);
}

/* A wrapper around a number so that when we pass it to an ostream, it prints
 * out an indent multiplied by that number, instead of the number. */
struct Indent {
    unsigned count;

    Indent(unsigned count) : count(count) { }
};

Indent operator+(Indent indent, unsigned count);

std::ostream& operator<<(std::ostream& os, Indent indent);

/* Strips the directory from the provided path, if present. */
std::string_view getBaseName(std::string_view path);

#endif /* UTIL_H */
