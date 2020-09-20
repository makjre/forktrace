/*  Copyright (C) 2021  Henry Harvey --- See LICENSE file
 *
 *  log
 *
 *      A whole bunch of nifty logging functions plus stuff for coloured text.
 */
#ifndef FORKTRACE_LOG_HPP
#define FORKTRACE_LOG_HPP

#include <string>
#include <fmt/core.h>
#include <fmt/format.h>

/* Describes types of log messages. We can ask the logger to filter these out
 * by their type so that we only see certain types of messages. */
enum class Log
{
    ERROR,  // For error messages from forktrace
    WARN,   // Warning messages
    LOG,    // general logging information
    VERB,   // verbose log messages
    DBG,    // debugging stuff (so not normally useful)
    NUM_LOG_CATEGORIES, // must be the last item in the list.
};

/* Should call at the very start of main. Just initializes some config to 
 * defaults that's too ugly / not possible to do as a static initializer.
 * Plus initializes a global containing the program name for us to access. 
 * Returns false and prints an error message if there's something wrong. */
bool init_log(const char* argv0);

/* Retrieve the stored program name. */
std::string_view program_name();

/* Sets whether the specified log category is enabled or not. This function is
 * thread-safe despite modifying global state (an atomic store). */
void set_log_category_enabled(Log category, bool enabled);

/* Returns true if the specified log type is enabled, based on the current
 * log options. Will load global state, but is thread safe (it's atomic). */
bool is_log_enabled_for(Log category);

/* Prints out a message. Differs from logging only in that it always prints no
 * matter what and cannot be disabled. Use this when we're printing input that
 * the user asked for or for interacting with the user (i.e., not logging!). 
 * Like the logging functions (message(), etc.) a newline is appended. */
void print_str(std::string_view str);

/* A helper function that saves us typing format() all the time */
template<typename ...Args>
inline void print(std::string_view fmtStr, Args... args)
{
    print_str(fmt::format(fmtStr, args...));
}

/* Always logs no matter the log level, otherwise the same as message(). Uses 
 * the category log level to (maybe) add a coloured prefix to the message, such
 * as "error: " or "warning: " (image the colours yourself!). */
void message_always(Log category, std::string_view message);

/* If is_log_enabled_for(level), prints out the message to stderr. Depending on
 * the log type, a different header may or may not be appended to the start of
 * the message. The message can have internal newlines. A newline will be added
 * to the end if there is no trailing newline. Doing this inline means that we
 * can avoid the cost of evaluating arguments when they won't be needed. */
inline void message(Log level, std::string_view message)
{
    if (is_log_enabled_for(level))
    {
        message_always(level, message);
    }
}
/* Same as message(), but defaults to Log::LOG. */
inline void message(std::string_view message)
{
    if (is_log_enabled_for(Log::LOG))
    {
        message_always(Log::LOG, message);
    }
}

/* Some helper functions that just forward a list of variadic arguments to
 * libfmt's format function. Saves a bit of typing (I'm THAT lazy). */
template <typename ...Args>
inline void log(std::string_view fmtStr, Args... args)
{
    message(Log::LOG, fmt::format(fmtStr, args...));
}

template <typename ...Args>
inline void warning(std::string_view fmtStr, Args... args)
{
    message(Log::WARN, fmt::format(fmtStr, args...));
}

template <typename ...Args>
inline void error(std::string_view fmtStr, Args... args)
{
    message(Log::ERROR, fmt::format(fmtStr, args...));
}

template <typename ...Args>
inline void verbose(std::string_view fmtStr, Args... args)
{
    message(Log::VERB, fmt::format(fmtStr, args...));
}

template <typename ...Args>
inline void debug(std::string_view fmtStr, Args... args)
{
    message(Log::DBG, fmt::format(fmtStr, args...));
}

/* A wrapper around a number so that when we pass it to a libfmt format string
 * it prints out an indent of some amount. */
struct Indent 
{
    unsigned count;
    Indent(unsigned count) : count(count) { }
    Indent operator+(Indent b) const { return Indent(count + b.count); }
};

/* Specialize libfmt's formatter struct so that it can accept our indentation
 * objects. https://fmt.dev/latest/api.html#formatting-user-defined-types */
template<>
struct fmt::formatter<Indent> : formatter<std::string>
{
    template <typename FormatContext>
    auto format(Indent indent, FormatContext& ctx)
    {
        return formatter<std::string>::format(
            std::string(indent.count * 4, ' '), ctx); // ezpz
    }
};

#endif /* FORKTRACE_LOG_HPP */
