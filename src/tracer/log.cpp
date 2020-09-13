/*  Copyright (C) 2021  Henry Harvey --- See LICENSE file
 *
 *  log
 *
 *      A whole bunch of nifty logging functions plus stuff for coloured text.
 */
#include <iostream>
#include <atomic>
#include <mutex>
#include <cassert>
#include <optional>

#include "log.hpp"
#include "util.hpp"

using std::string;
using std::string_view;
using std::optional;

/* Don't need this to be synchronized since we only write it once at the very
 * start of the program and it's read-only from there-on out. */
static string gProgramName;

/* Stores whether or not each log category is currently enabled or not... */
static std::atomic<bool> gLogCategoryEnabled[size_t(Log::NUM_LOG_CATEGORIES)];

/* Affects how colour() behaves. */
static std::atomic<bool> gColourEnabled = true;

// Colours other settings for our log messages
constexpr string_view PREFIX = "[forktrace] ";
constexpr fmt::text_style ERROR_COLOUR = fg(fmt::color::crimson) | fmt::emphasis::bold;
constexpr fmt::text_style WARNING_COLOUR = fg(fmt::color::medium_purple) | fmt::emphasis::bold;
constexpr fmt::text_style DEBUG_COLOUR = fg(fmt::color::gray) | fmt::emphasis::bold;

bool init_log(const char* argv0)
{
    set_log_category_enabled(Log::ERR, true);
    set_log_category_enabled(Log::WARN, true);
    set_log_category_enabled(Log::LOG, true);
    set_log_category_enabled(Log::VERB, false);
    set_log_category_enabled(Log::DBG, false);

    if (!argv0)
    {
        error("argv[0] is null??? Are you crazy?! Give me a name!");
        return false;
    }
    gProgramName = get_base_name(argv0);
    return true;
}

string_view program_name()
{
    return gProgramName;
}

void set_log_category_enabled(Log category, bool enabled)
{
    assert((Log)0 <= category && category < Log::NUM_LOG_CATEGORIES);
    gLogCategoryEnabled[size_t(category)] = enabled;
}

bool is_log_enabled_for(Log category)
{
    assert((Log)0 <= category && category < Log::NUM_LOG_CATEGORIES);
    return gLogCategoryEnabled[size_t(category)];
}

/* Internal helper function that does the work of message but allows you to
 * opt for no log category at all (otherwise the same as message()). */
void message_internal(optional<Log> logCategory, string_view message)
{
    string line;
    line.reserve(message.size() + 40);
    line += PREFIX;
    if (logCategory.has_value())
    {
        switch (logCategory.value())
        {
        case Log::ERR:
            line += colour(ERROR_COLOUR, "error: "); 
            break;
        case Log::WARN: 
            line += colour(WARNING_COLOUR, "warning: "); 
            break;
        case Log::DBG:
            line += colour(DEBUG_COLOUR, "debug: ");
            break;
        default:
            break;
        }
    }
    // Now we'll print out each line of the string separately (so that we can
    // attach our log prefix to the start of each line). Append a newline to
    // the final line if none is present.
    size_t pos = 0, next;
    while (next = message.find('\n', pos), next != string::npos)
    {
        line.append(message.substr(pos, next - pos + 1));
        std::cerr << line; // TODO cerr thread safe??!?!?!
        pos = next + 1;
        line = PREFIX; // start off the next line
    }
    if (pos < message.size())
    {
        line.append(message.substr(pos));
        line += '\n';
        std::cerr << line; // TODO cerr thread safe???!?!?
    }
}

void print_str(string_view message)
{
    message_internal({}, message);
}

void message_always(Log category, string_view message)
{
    message_internal({category}, message);
}

void set_colour_enabled(bool enabled)
{
    gColourEnabled = enabled;
}

string colour(const fmt::text_style& style, std::string_view str)
{
    if (!gColourEnabled)
    {
        return string(str);
    }
    return fmt::format(style, FMT_STRING("{}"), str);
}
