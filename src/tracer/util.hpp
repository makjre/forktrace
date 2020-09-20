/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  util
 *
 *      Functionality used by the whole project, e.g., string processing, 
 *      nifty macros, the program name, etc.
 */
#ifndef FORKTRACE_UTIL_HPP
#define FORKTRACE_UTIL_HPP

#include <string>
#include <vector>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Does the same as strerror, except it's thread-safe. Will return question
 * marks if an unknown error. Will also print out messages for the ERESTARTSYS
 * and ERESTARTNOINTR error codes (only visible to tracers). */
std::string strerror_s(int errnoVal);

/* Strips all leading and trailing whitespace from the specified string. */
void strip(std::string& str);

/* Returns true if 'a' starts/ends with 'b'. C++20 introduced these as methods
 * of string and string_view, but I don't want to depend on C++20. */
bool starts_with(std::string_view a, std::string_view b);
bool ends_with(std::string_view a, std::string_view b);

/* Pads a string out to some number using spaces. Makes things pretty! Will
 * ensure that there is at least one space at the end of the returned string.
 * This function will filter out ANSI escape sequences before calculating the
 * string length that it needs to pad to... :-) */
std::string pad(std::string s, size_t padding);

/* Joins the provided vector with the separator into a string. The separator
 * will only be placed in between items (not at the start or end). */
std::string join(const std::vector<std::string>& items, char sep = ' ');

/* Splits the provided string into tokens using the provided delimiter. Empty 
 * tokens will be ignored if skipEmpty is true. Ignoring empty tokens has the
 * same effect as merging consecutive occurrences of the delimiter. */
std::vector<std::string> split(std::string_view str,
                               char delim, 
                               bool skipEmpty = true);

/* Same as split (above) but returns a vector of string_views instead. These
 * string_views point to substrings of the argument `str` and are only valid
 * as long as `str` is also valid. This version prevents unnecessary copying.*/
std::vector<std::string_view> split_views(std::string_view str,
                                          char delim,
                                          bool skipEmpty = true);

/* Strips the directory from the provided path, if present. */
std::string_view get_base_name(std::string_view path);

/* If str contains spaces or non-printable characters, then it is returned as
 * a double-quoted string with C escape sequences (e.g., a newline is turned
 * into "\\n"). Otherwise, an unchanged string is returned. */
std::string escaped_string(std::string_view str);

#endif /* FORKTRACE_UTIL_HPP */
