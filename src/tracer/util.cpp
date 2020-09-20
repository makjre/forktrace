/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  util
 *
 *      Functionality used by the whole project, e.g., logging,
 *      string processing, nifty macros, etc.
 *
 *  dependencies:   nothing
 */
#include <cctype>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <regex>

#include "system.hpp"
#include "log.hpp"
#include "util.hpp"

using std::string;
using std::string_view;
using std::vector;

string strerror_s(int errnoVal)
{
    if (errnoVal == ERESTARTSYS)
    {
        return "ERESTARTNOINTR";
    }
    else if (errnoVal == ERESTARTNOINTR)
    {
        return "ERESTARTSYS";
    }

    // yes, there are "thread-safe" versions and a lock probably isn't needed,
    // but I took a look at the man page for two seconds and just couldn't be
    // bothered. god damn C, why are you so obsessed with global state. These
    // errno values should just be one static array of string literals anyway, 
    // right? Why can't the standard just guarantee that :-(
    static std::mutex lock;
    lock.lock();
    string err(strerror(errnoVal));
    lock.unlock();
    return err;
}

void strip(string& s) 
{
    auto notSpace = [](int c) { return !isspace(c); };
    // strip leading whitespace
    s.erase(s.begin(), find_if(s.begin(), s.end(), notSpace));
    // strip trailing whitespace
    s.erase(find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

bool starts_with(string_view a, string_view b)
{
    if (a.size() < b.size() || b.empty())
    {
        return false;
    }
    return strncmp(a.data(), b.data(), b.size()) == 0;
}

bool ends_with(string_view a, string_view b)
{
    if (a.size() < b.size() || b.empty())
    {
        return false;
    }
    return strncmp(&a[a.size() - b.size()], b.data(), b.size()) == 0;
}

string pad(string str, size_t padding)
{
    // Strip ANSI colour escapes so we can calculate length properly
    static std::regex re("\033\\[[;0-9]*[A-Za-z]");
    string bare = std::regex_replace(str, re, "");
    // Calculate how many spaces we need
    int deficit = padding - bare.size();
    // Ensure there's at least one space at the end of the string
    if (!bare.empty() && bare.back() != ' ' && deficit == 0)
    {
        deficit++;
    }
    str.reserve(padding);
    while (deficit-- > 0)
    {
        str += ' ';
    }
    return str;
}

string join(const vector<string>& items, char sep)
{
    // count size beforehand to be more efficient cos why not?!?!?!?!
    size_t total = 0;
    for (auto& str : items)
    {
        total += str.size();
    }
    string s;
    s.reserve(total + items.size()); // don't forget separators
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
        {
            s += sep;
        }
        s += items[i];
    }
    return s;
}

template<typename StrType>
vector<StrType> split_internal(string_view str, char delim, bool ignoreEmpty)
{
    vector<StrType> tokens;
    size_t start = 0; // start of current token
    size_t next; // start of next token
    while (next = str.find(delim, start), next != string::npos)
    {
        if (next > start || !ignoreEmpty)
        {
            tokens.push_back(StrType(str.substr(start, next - start)));
        }
        start = next + 1;
    }
    if (start < str.size() || !ignoreEmpty)
    {
        tokens.push_back(StrType(str.substr(start)));
    }
    return tokens;
}

vector<string> split(string_view str, char delim, bool ignoreEmpty)
{
    return split_internal<string>(str, delim, ignoreEmpty);
}

vector<string_view> split_views(string_view str, char delim, bool ignoreEmpty)
{
    return split_internal<string_view>(str, delim, ignoreEmpty);
}

string_view get_base_name(string_view path)
{
    size_t pos = path.rfind('/');
    if (pos != string::npos)
    {
        path.remove_prefix(pos + 1);
    }
    return path;
}
