/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  main
 *
 *      Basically just parsing for command line options. Once we've done all of
 *      that, we just pass everything over to forktrace() in forktrace.cpp.
 */
#include <cstring>
#include <iostream>
#include <cassert>
#include <functional>
#include <algorithm>
#include <optional>
#include <memory>

#include "util.hpp"
#include "log.hpp"
#include "forktrace.hpp"
#include "text-wrap.hpp"
#include "terminal.hpp"
#include "ptrace.hpp"
#include "parse.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::function;
using std::optional;
using std::unique_ptr;
using fmt::format;

/******************************************************************************
 * OPTION CLASSES (these represent command-line options)
 *****************************************************************************/

/* The callbacks for options can throw this if they encounter an error that
 * should cause parsing to stop and the program to exit with an error. */
class OptionError : public std::exception
{
private:
    string _msg;
public:
    OptionError(string_view msg) : _msg(msg) { }
    const char* what() const noexcept { return _msg.c_str(); }

    /* Helper constructor that accepts a fmt::format string and args */
    template<typename ...Args>
    OptionError(string_view fmt, Args ...args)
        : _msg(format(fmt, args...)) { }
};

struct Option
{
    /* Called by the ArgParser when this option is encountered. If a value is
     * provided for the option, it is passed - otherwise nullopt is passed. 
     * This function should throw a std::exception to indicate an error. */
    virtual void parse(optional<string> param) const = 0;

    string name;    // Long form name. Doesn't include double dashes.
    char shortName; // == '\0' if there's no short form.
    string param;   // name of arguments, empty string if not applicable
    string help;    // Doesn't need newlines. We'll do that with wrap_text.

    Option(string_view name, 
               char shortName,
               string_view param, 
               string_view help)
        : name(name), shortName(shortName), param(param), help(help) { }

    virtual ~Option() { }
};

/* A command-line option that takes no parameter. */
struct Option0 : Option
{
    void parse(optional<string> param) const;

    function<void()> handler; // callback for this option

    Option0(string_view name, 
           char shortName, 
           string_view param,
           string_view help, 
           function<void()> handler)
        : Option(name, shortName, param, help), handler(handler) { }
};

/* A command-line option that takes a string parameter (however, this parameter
 * could be a list of multiple items separated by commas or something). */
struct Option1 : Option
{
    void parse(optional<string> param) const;

    function<void(string)> handler; // callback for this option

    Option1(string_view name, 
           char shortName, 
           string_view param,
           string_view help, 
           function<void(string)> handler)
        : Option(name, shortName, param, help), handler(handler) { }
};

void Option0::parse(optional<string> param) const
{
    if (param.has_value())
    {
        throw OptionError("Option \"--{}\" expects no value.", name);
    }
    handler();
}

void Option1::parse(optional<string> param) const
{
    if (!param.has_value())
    {
        throw OptionError("Option \"--{}\" expects a value.", name);
    }
    handler(std::move(param.value()));
}

/* Describes a group of command line options. Used internally by ArgParser. */
struct OptionGroup
{
    string name;
    vector<unique_ptr<Option>> options;

    OptionGroup(string_view name) : name(name) { }
};

/******************************************************************************
 * ARGPARSER CLASS
 *****************************************************************************/

/* Provides a nice way to parse command line options automatically. Allows us
 * to register our options with it via callbacks and whatnot.  
 * The parser also hardcodes the options for help (-h and --help). */
class ArgParser
{
private:
    /* Our list of options (split into groups). Could try to use a map<> or
     * something but who cares about efficient argument parsing! */
    vector<OptionGroup> _groups;
    /* Should we exit the program after parsing the arguments (e.g., help flag) */
    bool _doExit;
    /* Excludes argv[0]. We're going to be maintaining pointers into this as we
     * move along and parse everything, so better not resize or modfiy it!. */
    vector<string> _args;
    size_t _pos; // index of current argument within the vector

    /* Helps us move along the arguments list. current() returns the current
     * one and next() moves us to the next one (and returns it). Both of these
     * return nullopt if the end of the list of arguments has been passed. */
    optional<string> current() const;
    optional<string> next();

    /* Helps us find options using different names. Returns null if none. */
    const Option* find(string_view name) const;
    const Option* find(char shortName) const;

    /* Prints out the help for all of the program options. */
    void print_help() const;

    /* Internal parsing functions */
    bool parse_long_flag(string flag);
    bool parse_short_flags(string flags);
    bool parse_flag(string flag);

    /* Internal version of parse() that does all the work. This assumes that
     * _args and _pos have been set up and that _args is non-empty. */
    bool parse_internal();

public:
    ArgParser();

    /* All the options added after this (until the next start_new_group call)
     * will be placed under the provided group name. Returns *this. */
    void start_new_group(string_view groupName);

    /* Add a command-line option that either does or doesn't take a param and
     * that either does or doesn't have a shortName. I could reduce the number
     * of overloads if I used default parameters, but that would require me to
     * put those parameters last, which I don't want to do. */
    void add(string_view name, 
             char shortName, 
             string_view param,
             string_view help, 
             function<void()> handler);

    void add(string_view name, 
             string_view param,
             string_view help, 
             function<void()> handler);

    void add(string_view name, 
             char shortName, 
             string_view param,
             string_view help, 
             function<void(string)> handler);

    void add(string_view name, 
             string_view param,
             string_view help, 
             function<void(string)> handler);

    /* Parses the provided argv array (a null-terminated array of arguments,
     * including argv[0]). argv must contain >= 1 arguments. When done parsing,
     * the function returns false if there was an error parsing the arguments. 
     * Left-over arguments are stored in the provided vector (someone else can
     * parse these non-flag arguments). */
    bool parse(const char* argv[], vector<string>& remainingArgs);

    /* Returns true if the program should exit */
    bool should_exit() { return _doExit; }

    /* Option callbacks can call this if they don't want the program to run. */
    void schedule_exit() { _doExit = true; }
};

ArgParser::ArgParser() : _doExit(false), _pos(0)
{
    string me(program_name());
    start_new_group("");
    add("help", 'h', "", "displays this help message",
        [&]{ print_help(); schedule_exit(); });
}

optional<string> ArgParser::current() const
{
    return _pos >= _args.size() 
        ? optional<string>() : optional<string>(_args[_pos]);
}

optional<string> ArgParser::next()
{
    _pos++;
    return current();
}

const Option* ArgParser::find(string_view name) const
{
    for (const OptionGroup& group : _groups)
    {
        for (const unique_ptr<Option>& option : group.options)
        {
            if (option->name == name)
            {
                return option.get();
            }
        }
    }
    return nullptr;
}

const Option* ArgParser::find(char shortName) const
{
    for (const OptionGroup& group : _groups)
    {
        for (const unique_ptr<Option>& option : group.options)
        {
            if (option->shortName != '\0' && option->shortName == shortName)
            {
                return option.get();
            }
        }
    }
    return nullptr;
}

static bool is_valid_name(string_view name)
{
    auto isBad = [](char c) { return !isalnum(c) && c != '_' && c != '-'; };
    return std::find_if(name.begin(), name.end(), isBad) == name.end();
}

void ArgParser::start_new_group(string_view groupName)
{
    _groups.emplace_back(groupName);
}

void ArgParser::add(string_view name, 
                          char shortName, 
                          string_view param,
                          string_view help, 
                          function<void()> handler)
{
    assert(is_valid_name(name));
    assert(!find(name));
    assert(!find(shortName));
    assert(!_groups.empty());
    vector<unique_ptr<Option>>& options = _groups.back().options;
    options.push_back(std::make_unique<Option0>(
        name, shortName, param, help, handler
    ));
    std::sort(options.begin(), options.end(), 
        [](unique_ptr<Option>& a, unique_ptr<Option>& b) { 
            return a->name.compare(b->name) < 0; }); // yeet
}

void ArgParser::add(string_view name, 
                          string_view param,
                          string_view help, 
                          function<void()> handler)
{
    add(name, '\0', param, help, handler);
}

void ArgParser::add(string_view name, 
                          char shortName, 
                          string_view param,
                          string_view help, 
                          function<void(string)> handler)
{
    assert(is_valid_name(name));
    assert(!find(name));
    assert(!find(shortName));
    assert(!_groups.empty());
    vector<unique_ptr<Option>>& options = _groups.back().options;
    options.push_back(std::make_unique<Option1>(
        name, shortName, param, help, handler
    ));
    std::sort(options.begin(), options.end(), 
        [](unique_ptr<Option>& a, unique_ptr<Option>& b) { 
            return a->name.compare(b->name) < 0; }); // yeet
}

void ArgParser::add(string_view name, 
                          string_view param,
                          string_view help, 
                          function<void(string)> handler)
{
    add(name, '\0', param, help, handler);
}

void ArgParser::print_help() const
{
    string_view me = program_name();
    std::cerr 
        << "Start up a command prompt (interactive mode):\n"
        << "  " << me << " [OPTIONS...]\n"
        << "\n"
        << "Directly run a program in forktrace (instant mode):\n"
        << "  " << me << " [OPTIONS...] [--] program [ARGS...]\n"
        << "\n"
        << "Use '--' to force " << me << " to stop parsing flags.\n";

    size_t width = 0, height = 0;
    get_terminal_size(width, height); // this could fail (and return false)

    for (const OptionGroup& group : _groups)
    {
        // Calculate padding we'll need to line up the help messages for each
        // command in this category nicely :-)
        size_t padding = 0;
        for (const unique_ptr<Option>& opt : group.options)
        {
            size_t width = 2 + opt->name.size();
            if (!opt->param.empty())
            {
                width += 1 + opt->param.size();
            }
            if (opt->shortName != '\0')
            {
                width += 3;
            }
            padding = std::max(padding, width);
        }
        padding += 2;

        std::cerr << group.name << '\n';
        for (const unique_ptr<Option>& opt : group.options)
        {
            string line;
            if (opt->shortName != '\0')
            {
                line += colour(Colour::BOLD, format("-{} ", opt->shortName));
            }
            line += colour(Colour::BOLD, format("--{}", opt->name));
            if (!opt->param.empty())
            {
                line += '=' + opt->param;
            }
            line = "  " + pad(line, padding); // ensures trailing space

            if (width == 0 || line.size() + opt->help.size() > width)
            {
                std::cerr << line << '\n';
                std::cerr << wrap_text(opt->help, width, 4);
            }
            else
            {
                std::cerr << line << opt->help << '\n';
            }
        }
        std::cerr << '\n';
    }
}

bool ArgParser::parse_long_flag(string flag)
{
    assert(starts_with(flag, "--"));
    flag = flag.substr(2);

    optional<string> value;
    size_t equalsPos = flag.find('=');
    if (equalsPos != string::npos)
    {
        value = {flag.substr(equalsPos + 1)};
        flag = flag.substr(0, equalsPos);
    }

    const Option* opt = find(flag);
    if (!opt)
    {
        error("The \"--{}\" flag doesn't exist.", flag);
        return false;
    }
    opt->parse(value);
    return true;
}

bool ArgParser::parse_short_flags(string flags)
{
    assert(starts_with(flags, "-"));
    for (size_t i = 1; i < flags.size(); ++i)
    {
        const Option* opt = find(flags[i]);
        if (!opt)
        {
            error("The '-{}' flag doesn't exist.", flags[i]);
            return false;
        }
        if (i + 1 < flags.size() && flags[i + 1] == '=')
        {
            string value = flags.substr(i + 2);
            opt->parse({value});
            return true;
        }
        opt->parse({});
    }
    return true;
}

bool ArgParser::parse_flag(string flag)
{
    assert(starts_with(flag, "-"));
    try
    {
        if (starts_with(flag, "--"))
        {
            return parse_long_flag(std::move(flag));
        }
        else if (flag.size() > 1)
        {
            return parse_short_flags(std::move(flag));
        }
        else
        {
            error("Invalid argument '-'.");
            return false;
        }
    }
    catch (const std::exception& e)
    {
        error("{}", e.what());
        return false;
    }
}

/* This function assumes _args is non-empty and _pos has been set to 0. */
bool ArgParser::parse_internal()
{
    do
    {
        string arg = current().value();
        // The separator forces us to stop parsing command line options.
        if (arg == "--")
        {
            next(); // skip the separator
            return true;
        }

        if (starts_with(arg, "-"))
        {
            if (!parse_flag(std::move(arg)))
            {
                return false; // invalid option
            }
        }
        else
        {
            return true; // not a flag - stop parsing here
        }
    } 
    while (next());
    return true;
}

bool ArgParser::parse(const char* argv[], vector<string>& remainingArgs)
{
    _doExit = false;
    _args.clear();
    _pos = 0;

    assert(argv[0] != nullptr);
    for (const char** argp = &argv[1]; *argp != nullptr; ++argp)
    {
        _args.emplace_back(*argp);
    }
    if (_args.empty())
    {
        return true;
    }

    bool success = parse_internal();

    // Erase all the arguments up to where we finished parsing so that
    // we can return those leftover arguments to the caller.
    assert(_pos <= _args.size());
    _args.erase(_args.begin(), _args.begin() + _pos);
    remainingArgs = std::move(_args);

    return success;
}

/******************************************************************************
 * COMMAND LINE OPTIONS & MAIN()
 *****************************************************************************/

/* This feature is useful for debugging */
static void diagnose_status(int wstatus)
{
    std::cerr << diagnose_wait_status(wstatus) << '\n';
}

/* This feature is useful for debugging */
static void print_syscall(int syscall)
{
    std::cerr << format("{} ({} args)\n", get_syscall_name(syscall), 
        get_syscall_arg_count(syscall));
}

/* Registers all of our command line options with the argparser. */
static void register_options(ArgParser& parser, Forktrace::Options& opts)
{
    parser.add("no-colour", 'c', "", "disables colours", 
        []{ set_colour_enabled(false); }
    );
    parser.add("no-reaper", "", "disables the sub-reaper process",
        [&]{ opts.reaper = false; }
    );
    parser.add("status", "STATUS", "diagnose a wait(2) child status",
        [&](string s) { diagnose_status(parse_number<int>(s)); parser.schedule_exit(); }
    );
    parser.add("syscall", "NUMBER", "print info about a syscall number",
        [&](string s) { print_syscall(parse_number<int>(s)); parser.schedule_exit(); }
    );

    parser.start_new_group("Diagram options");

    parser.add("scroll-view", 's', "", 
        "always opt for the scroll-view when in instant mode",
        [&]{ opts.forceScrollView = true; }
    );
    parser.add("non-fatal", "yes|no", "show or hide non-fatal signals",
        [&](string s) { opts.showNonFatalSignals = parse_bool(s); }
    );
    parser.add("execs", "yes|no", "show or hide successful execs",
        [&](string s) { opts.showExecs = parse_bool(s); }
    );
    parser.add("bad-execs", "yes|no", "show or hide failed execs",
        [&](string s) { opts.showFailedExecs = parse_bool(s); }
    );
    parser.add("signal-sends", "yes|no", "show or hide sent signals",
        [&](string s) { opts.showSignalSends = parse_bool(s); }
    );
    parser.add("merge-execs", "yes|no", 
        "if true, merge retried execs of the same program",
        [&](string s) { opts.mergeExecs = parse_bool(s); }
    );
    parser.add("lane-width", "WIDTH", "set the diagram lane width",
        [&](string s) { opts.laneWidth = parse_number<size_t>(s); }
    );

    parser.start_new_group("Logging options");

    parser.add("verbose", 'v', "", "shows more information than usual",
        []{ set_log_category_enabled(Log::VERB, true); }
    );
    parser.add("debug", 'd', "", "shows debugging log messages", 
        []{ set_log_category_enabled(Log::DBG, true); }
    );
    parser.add("no-log", 'l', "", "disable general log messages",
        []{ set_log_category_enabled(Log::LOG, false); }
    );
}

int main(int argc, const char** argv) 
{
    try
    {
        if (!init_log(argv[0])) // makes sure argv[0] isn't null :-)
        {
            return 1;
        }

        Forktrace::Options opts;
        ArgParser parser;
        register_options(parser, opts);

        vector<string> remainingArgs;
        if (!parser.parse(argv, remainingArgs))
        {
            return 1;
        }
        if (parser.should_exit())
        {
            return 0;
        }
        return forktrace(std::move(remainingArgs), opts) ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        error("Fatal! Got unhandled exception: {}", e.what());
        return 1;
    }
}
