#include <fmt/core.h>

#include "inject.hpp"
#include "util.hpp"
#include "log.hpp"

using std::string;
using std::vector;
using fmt::format;

bool do_inject(vector<string> files, vector<string> argv)
{
    verbose("The compile command is: {}", join(argv));
    verbose("Files to inject: {}", join(files));
    error("This feature is not implemented yet.");
    return false;
}
