/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  parse
 *
 *      TODO
 */
#ifndef FORKTRACE_PARSE_HPP
#define FORKTRACE_PARSE_HPP

#include <string>
#include <charconv>
#include <fmt/core.h>

/* The parse functions will throw this error whenever they fail. */
class ParseError : public std::exception
{
private:
    std::string _msg;
public:
    ParseError(std::string_view msg) : _msg(msg) { }
    const char* what() const noexcept { return _msg.c_str(); }
};

/* Parses a boolean argument and throws an exception if the argument is not
 * valid. Accepts things like enabled/disabled, yes/no, true/false, 0/1 */
bool parse_bool(std::string_view input);

/* Helper function to parse arbitrary integer argments. The entire string must
 * be a valid integer, otherwise an exception will be thrown. */
template<class T>
T parse_number(std::string_view input) 
{
    T value;
    const auto result = std::from_chars(input.data(),
            input.data() + input.size(), value);
    if (result.ptr == input.data()
        || result.ptr != input.data() + input.size()
        || result.ec != std::errc()) 
    {
        throw ParseError(fmt::format("'{}' is not a valid number.", input));
    }
    return value;
}

#endif /* FORKTRACE_PARSE_HPP */
