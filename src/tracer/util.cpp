#include <fstream>
#include <cctype>
#include <algorithm>
#include <iostream>

#include "util.h"

using namespace std;

/* A black hole. */
ostream null(0);

ostream tlog(cout.rdbuf());
ostream dbg(null.rdbuf());
ostream vdbg(null.rdbuf());

static unsigned debugLogLevel = 0;

void setDebugLogLevel(unsigned level) {
    switch (level) {
        case 0:
            dbg.rdbuf(null.rdbuf());
            vdbg.rdbuf(null.rdbuf());
            break;
        case 1:
            dbg.rdbuf(cerr.rdbuf());
            vdbg.rdbuf(null.rdbuf());
            break;
        case 2:
            dbg.rdbuf(cerr.rdbuf());
            vdbg.rdbuf(cerr.rdbuf());
            break;
        default:
            throw runtime_error("Invalid debug logging level.");
    }
    debugLogLevel = level;
}

void setLogEnabled(bool enabled) {
    if (enabled) {
        tlog.rdbuf(cout.rdbuf());
        setDebugLogLevel(debugLogLevel);
    } else {
        tlog.rdbuf(null.rdbuf());
        dbg.rdbuf(null.rdbuf());
        vdbg.rdbuf(null.rdbuf());
    }
}

void strip(std::string& s) {
    auto notSpace = [](int c) { return !isspace(c); };

    // strip leading whitespace
    s.erase(s.begin(), find_if(s.begin(), s.end(), notSpace));

    // strip trailing whitespace
    s.erase(find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

Indent operator+(Indent indent, unsigned count) {
    return Indent(indent.count + count);
}

ostream& operator<<(ostream& os, Indent indent) {
    for (unsigned i = 0; i < indent.count; ++i) {
        os << "    ";
    }
    return os;
}

string_view getBaseName(string_view path) {
    size_t pos = path.find_last_of('/');
    if (pos != string::npos) {
        path.remove_prefix(pos + 1);
    }
    return path;
}
