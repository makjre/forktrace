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
#include <map>
#include <memory>

#include "util.hpp"
#include "log.hpp"
#include "inject.hpp"
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
using std::map;
using std::unique_ptr;
using fmt::format;

/* Returned by argument parser to determine what course of action we should be
 * taking. Depending on the action, the argument parser may leave some args
 * unparsed and leave them to the specific action to handle. */
enum class ProgramAction
{
    /* Take a command run the command line, run it from start to finish, and
     * then exit the program (although the user can try to halt the process,
     * in which case the RUN is cancelled and they get taken to the CMDLINE).
     *
     * When the run flag is specified, the argparser keeps looking for more
     * options after it, and parsing them, but when it hits an argument that
     * isn't an option (i.e., doesn't start with '-') (and doesn't belong to 
     * an option) it assumes that it's hit the start of the command that needs 
     * to be run, and leaves the rest of the parsing to the RUN action. If a
     * separator consisting of two dashes "--" is hit, then the argparser stops
     * anyway and leaves any remaining arguments to the RUN action. */
    RUN,
    /* In this case, the program just starts up the command line immediately
     * and lets the user take it from there. This is the default behaviour of
     * the program if no arguments are given. In this case, the argument parser
     * parses the entire command line for options. */
    CMDLINE,
    /* This is the mode (see inject_option_usage for a long discussion) that
     * helps modify compile commands so that the output files have extra code
     * in them that helps forktrace identify the source location of certain
     * system calls. When the inject flag is specified, the argument parser 
     * stops. No further options are parsed since they are assumed to either
     * be the names of files to inject, or arguments to the compiler command
     * to inject into. All remaining parsing is left to the INJECT code. */
    INJECT,
    /* There was an error parsing the command line arguments (so the program
     * should exit with an error status, e.g., 1). */
    ERROR,
    /* The program should just exit straight away (this would happen for the
     * help option). The program should not exit with an error status. */
    EXIT,
};

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
 * to register our options with it via callbacks and whatnot. Exceptions are
 * the options determining ProgramAction (-i, -r, --inject, --run): these are
 * hardcoded into the parser (those options affect the structure of the command
 * line itself, and I don't want to over-engineer this by abstracting that). 
 * The parser also hardcodes the options for help (-h and --help). */
class ArgParser
{
private:
    /* Our list of options (split into groups). Could try to use a map<> or
     * something but who cares about efficient argument parsing! */
    vector<OptionGroup> _groups;
    /* See the enum definition. At each point in the argument parsing, this is
     * the current action that the program is determined to take based on what
     * we've seen so far. This forms somewhat of a state machine as we're doing
     * the parsing (e.g., the value dictates what is valid after it). The flags
     * that determine the program action get special treatment by the parser.*/
    ProgramAction _action;
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

    /* Sets the program action to something. Does some checking to see if the
     * new action clashes with the current value and throws an OptionError if
     * it does clash. It also manages priority of the actions: so, e.g., if the
     * current action is EXIT then that can only be replaced with ERROR. */
    void set_action(ProgramAction newAction);

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
     * the function stores the action that the program should take from then
     * on (which could be ProgramAction::ERROR if there was an error parsing
     * the arguments) and returns a vector of the remaining arguments to be
     * parsed (the parser may stop parsing before the end of argv depending on
     * the action -- see the comments for enum ProgramAction). */
    vector<string> parse(const char* argv[], ProgramAction& action);

    /* Option callbacks can call this if they don't want the program to run. */
    void exit() { set_action(ProgramAction::EXIT); }
};

ArgParser::ArgParser() : _action(ProgramAction::CMDLINE), _pos(0)
{
    string me(program_name());
    start_new_group("");
    add("help", 'h', "", "displays this help message",
        [&]{ print_help(); set_action(ProgramAction::EXIT); });
    add("inject", 'i', "", "type '" + me + " -i' for more information",
        [&]{ set_action(ProgramAction::INJECT); });
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
    _groups.back().options.push_back(std::make_unique<Option0>(
        name, shortName, param, help, handler
    ));
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
    _groups.back().options.push_back(std::make_unique<Option1>(
        name, shortName, param, help, handler
    ));
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
        << "Compile a program so that " << me << " can get more information:\n"
        << "  " << me << " [OPTIONS...] -i [FILES...] -- compiler {ARGS...}\n"
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

void ArgParser::set_action(ProgramAction action)
{
    if ((action == ProgramAction::RUN && _action == ProgramAction::INJECT)
        || (action == ProgramAction::INJECT && _action == ProgramAction::RUN))
    {
        throw OptionError("The run and inject options are mutually exclusive.");
    }
    if (_action == ProgramAction::ERROR)
    {
        return; // can't override this one
    }
    if (_action == ProgramAction::EXIT)
    {
        return; // next highest priority
    }
    _action = action;
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

        if (_action == ProgramAction::INJECT)
        {
            // If we hit an inject flag, then we'll let the INJECT code handle
            // the rest of the program's arguments.
            next(); // skip the inject flag
            return true;
        }
    } 
    while (next());
    return true;
}

vector<string> ArgParser::parse(const char* argv[], ProgramAction& action)
{
    _action = ProgramAction::CMDLINE; // default action
    _args.clear();
    _pos = 0;

    assert(argv[0] != nullptr);
    for (const char** argp = &argv[1]; *argp != nullptr; ++argp)
    {
        _args.emplace_back(*argp);
    }
    if (_args.empty())
    {
        action = ProgramAction::CMDLINE;
        return {};
    }

    bool success = parse_internal();

    // Erase the arguments up until where we finished parsing so that we can
    // return the remaining arguments to the caller.
    assert(_pos <= _args.size());
    _args.erase(_args.begin(), _args.begin() + _pos);

    // Set ourselves to run mode if they gave us a command to run.
    if (_action == ProgramAction::CMDLINE && !_args.empty())
    {
        _action = ProgramAction::RUN;
    }

    action = (success ? _action : ProgramAction::ERROR);
    return std::move(_args); // will clear _args
}

/******************************************************************************
 * INJECT ACTION
 *****************************************************************************/

/* Print the description of the inject option. */
static void inject_option_usage()
{
    string programName(program_name());
    std::cerr << "usage: " << programName
        << " [OPTIONS...] -i [FILES...] -- compiler {ARGS...}\n\n";

    string text1 = 
    "The inject option takes a compiler command and runs it, while injecting "
    "code into each of the source files so that " + programName + " can "
    "trace the source locations where all the events happen (yes, programs "
    "like gdb don't need to do this, but doing what gdb does takes a lot of "
    "effort to implement)."
    "\n\n"
    "The injected code is done with #includes, so will only work on the "
    "source files. This option only supports C/C++. By default, the inject "
    "option will search for files in the compiler arguments with the "
    "following extensions:";
    string text2 = ".c .h .cpp .hpp .cc .hh .cxx .hxx";
    string text3 =
    "Note that this option will not modify the specified source files (not "
    "even temporarily). It uses some ptrace magic to alter what the compiler "
    "reads."
    "\n\n"
    "The inject option should be separated from the compiler command with two "
    "dashes ('--'). In between the -i option and these dashes, you can insert "
    "names of files (in the compiler command) that should be injected. If any "
    "files are specified, then they override the default behaviour seen above."
    "\n\n"
    "The injections use preprocessor macros to insert a special syscall with "
    "an invalid syscall number after each of the relevant functions which "
    + programName + " can latch onto. This syscall will silently fail in "
    "normal operation. Since ptracing is on a per-thread basis, this works "
    "for multi-threaded programs."
    "\n\n"
    "The following syscalls (actually, the C library wrappers for them) are "
    "traced:";
    string text4 =
    "fork, kill, raise, tkill, tgkill, wait, waitpid, waitid, wait3, " 
    "wait4, execv, execvp, execve, execl, execlp, execle, execvpe";

    std::cerr << wrap_text_to_screen(text1, true, 0, 79) << '\n' 
        << wrap_text_to_screen(text2, false, 4, 79) << '\n'
        << wrap_text_to_screen(text3, true, 0, 79) << '\n' 
        << wrap_text_to_screen(text4, false, 4, 79) << '\n';
}

/* Searches the arguments of the provided compiler argv list for files that
 * contain C/C++ file extensions (which we will inject into) and return a list
 * of those items. Will skip argv[0] when searching (i.e., the compiler). */
static vector<string> find_inject_files(const vector<string>& argv)
{
    vector<string_view> exts = {
        ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".cxx", ".hxx"
    };
    vector<string> files;
    for (size_t i = 1; i < argv.size(); ++i)
    {
        for (string_view ext : exts)
        {
            if (ends_with(argv.at(i), ext))
            {
                files.push_back(argv.at(i));
            }
        }
    }
    return files;
}

/* Called when forktrace was called with the inject option. args must contain
 * all of the command-line arguments after the -i flag. On succes, the inject 
 * action is done. On failure, false is returned. */
static bool handle_inject_action(vector<string> args)
{
    // Split the remaining arguments using the separator into two vectors
    // usage: forktrace -i [FILES...] -- compiler {ARGS...}
    // We'll expect the compiler command to have at least one argument
    auto sep = std::find(args.begin(), args.end(), "--");
    if (sep == args.end())
    {
        inject_option_usage();
        return false;
    }
    vector<string> files(args.begin(), sep);
    vector<string> cmd(sep + 1, args.end());

    // We expect the compile command to have at least one parameter
    if (cmd.size() < 2) // code further down relies on this
    {
        inject_option_usage();
        return false;
    }
    for (auto it = files.begin(); it != files.end();)
    {
        if (!it->empty() && (*it)[0] == '-')
        {
            warning("'{}' looks like it's supposed to be an option, but "
                "it's being interpreted as a file.", *it);
        }
        // can skip argv[0] since we checked earlier that cmd.size() >= 2
        if (std::find(cmd.begin() + 1, cmd.end(), *it) == cmd.end())
        {
            warning("File '{}' was listed to be injected but wasn't found "
                "in the compiler's arguments. Ignoring.", *it);
            it = files.erase(it); // Iterator now points to next item.
            if (files.empty())
            {
                error("No files to inject. Stopping.");
                return false;
            }
            continue;
        }
        ++it;
    }
    if (files.empty())
    {
        files = find_inject_files(cmd); // search based on file extensions
    }
    // Now pass the parsed arguments off for the magic to happen
    return do_inject(std::move(files), std::move(cmd));
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
static void register_options(ArgParser& parser, ForktraceOpts& opts)
{
    parser.add("no-colour", 'c', "", "disables colours", 
        []{ set_colour_enabled(false); }
    );
    parser.add("no-reaper", "", "disables the sub-reaper process",
        [&]{ opts.reaper = false; }
    );
    parser.add("status", "STATUS", "diagnose a wait(2) child status",
        [&](string s) { diagnose_status(parse_number<int>(s)); parser.exit(); }
    );
    parser.add("syscall", "NUMBER", "print info about a syscall number",
        [&](string s) { print_syscall(parse_number<int>(s)); parser.exit(); }
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
}

static bool do_all_the_things(int argc, const char** argv)
{
    if (!init_log(argv[0])) // makes sure argv[0] isn't null :-)
    {
        return false;
    }

    ForktraceOpts opts;
    ArgParser parser;
    register_options(parser, opts);

    ProgramAction action;
    vector<string> remainingArgs = parser.parse(argv, action);

    switch (action)
    {
    case ProgramAction::RUN:
    case ProgramAction::CMDLINE:
        return forktrace(std::move(remainingArgs), opts);
    case ProgramAction::INJECT:
        return handle_inject_action(std::move(remainingArgs));
    case ProgramAction::EXIT:
        return true;
    case ProgramAction::ERROR:
        return false;
    default:
        assert(!"Unreachable");
    }
}

int main(int argc, const char** argv) 
{
    return do_all_the_things(argc, argv) ? 0 : 1;
}
