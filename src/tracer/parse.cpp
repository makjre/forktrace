/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  parse
 *
 *      TODO
 */
#include <algorithm>
#include <cctype>

#include "parse.hpp"

using std::string;
using std::string_view;
using fmt::format;

bool parse_bool(string_view input) 
{
    string str(input);
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);

    if (str == "yes" || str == "1" || str == "on" || str == "enabled" 
        || str == "enable" || str == "true") 
    {
        return true;
    }
    if (str == "no" || str == "0" || str == "off" || str == "disabled"
        || str == "disable" || str == "false") 
    {
        return false;
    }
    throw ParseError(format("'{}' is not a valid boolean.", input));
}
