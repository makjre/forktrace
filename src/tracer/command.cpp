/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  command
 *
 *      TODO
 */
#include <signal.h>
#include <cassert>
#include <iostream>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "command.hpp"
#include "log.hpp"
#include "util.hpp"
#include "text-wrap.hpp"
#include "terminal.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::function;
using std::optional;
using fmt::format;

/* Will install this SIGINT handler when reading a line so that pressing Ctrl+C
 * will cancel the current line. Idk if this is what you're supposed to do...*/
static void read_line_sigint_handler(int sig)
{
    write(1, "\n", 1);
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

bool read_line(string_view prompt, string& line, bool complete)
{
    if (complete)
    {
        rl_bind_key('\t', rl_complete);
    }
    else
    {
        rl_unbind_key('\t');
    }

    struct sigaction sa = {0}, old;
    sa.sa_flags = 0;
    sa.sa_handler = read_line_sigint_handler;
    sigaction(SIGINT, &sa, &old);

    // make sure at least one thread (this one) can handle SIGINT
    // TODO this is a dependency between command.cpp and forktrace.cpp. 
    // Can you retrieve current signal mask and restore that instead so
    // that it's more modular and other parts of the code don't depend on this?
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

    string promptStr(prompt);
    char* input = readline(promptStr.c_str()); // grrr c strings

    // restore things back to the way they were before
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    sigaction(SIGINT, &old, nullptr);

    if (!input) {
        return false;
    }

    line.assign(input);
    strip(line);

    if (!line.empty())
    {
        add_history(input);
    }
    free(input);
    return true;
}

CommandParser::CommandParser()
{
    start_new_group("");
    add("help", "[COMMAND]", 
        "shows help about a command, or all if none specified", 
        [&](vector<string> args) { help_handler(args); });
}

const CommandParser::Command* 
CommandParser::find_command(string_view prefix) const
{
    vector<string> names;
    vector<const Command*> matches;
    for (const Group& group : _groups)
    {
        for (const Command& command : group.commands)
        {
            if (command.name == prefix) // exact match
            {
                return &command;
            }
            if (starts_with(command.name, prefix))
            {
                names.push_back(command.name);
                matches.push_back(&command);
            }
        }
    }
    if (matches.empty())
    {
        error("Couldn't find any commands matching '{}'.", prefix);
        return nullptr;
    }
    if (matches.size() > 1)
    {
        error("'{}' is ambiguous. Options are: {}", prefix, join(names));
        return nullptr;
    }
    return matches.front();
}

void CommandParser::print_help(const CommandParser::Group& group) const
{
    if (!group.name.empty())
    {
        std::cerr << '\n' << group.name << '\n';
    }

    // Calculate the largest width of command name + argments so that we know
    // what to pad the command help strings to to make it look pretty.
    size_t padding = 0;
    for (const Command& command : group.commands)
    {
        padding = std::max(padding, 
            command.name.size() + 1 + command.params.size());
    }
    padding += 2;

    for (const Command& command : group.commands)
    {
        size_t width = 0, height = 0;
        get_terminal_size(width, height); // this could fail (and return false)

        string line = colour(Colour::BOLD, command.name);
        if (!command.params.empty())
        {
            line += ' ' + command.params;
        }
        line = "  " + pad(line, padding);

        if (width == 0 || line.size() + command.help.size() > width)
        {
            std::cerr << line << '\n';
            std::cerr << wrap_text(command.help, width, 8);
        }
        else
        {
            std::cerr << line << command.help << '\n';
        }
    }
}

void CommandParser::help_handler(vector<string> args) const
{
    if (args.size() > 1)
    {
        error("The 'help' command accepts either zero or one arguments.");
        return;
    }
    if (args.size() == 1)
    {
        const Command* command = find_command(args[0]);
        if (!command)
        {
            return; // find_command prints an error for us
        }
        string cmd = colour(Colour::BOLD, command->name);
        std::cerr << format("{} {}\n", cmd, command->params);
        std::cerr << wrap_text_to_screen(command->help, false, 0);
        return;
    }
    std::cerr << '\n';
    for (const Group& group : _groups)
    {
        print_help(group);
    }
    std::cerr << '\n';
}

static bool is_valid_name(string_view name)
{
    auto isBad = [](char c) { return !isalnum(c) && c != '_' && c != '-'; };
    return std::find_if(name.begin(), name.end(), isBad) == name.end();
}

void CommandParser::start_new_group(string_view name)
{
    _groups.emplace_back(name);
}

void CommandParser::add(string_view name,
                        string_view params,
                        string_view help,
                        function<void()> action,
                        bool autoRepeat)
{
    auto wrapper = [=](vector<string> args) {
        if (!args.empty())
        {
            error("The '{}' command expects no arguments.", name);
            return;
        }
        action();
    };
    return add(name, params, help, wrapper, autoRepeat);
}

void CommandParser::add(string_view name,
                        string_view params,
                        string_view help,
                        function<void(string)> action,
                        bool autoRepeat)
{
    auto wrapper = [=](vector<string> args) {
        if (args.size() != 1)
        {
            error("The '{}' command expects a single argument.", name);
            return;
        }
        action(std::move(args[0]));
    };
    return add(name, params, help, wrapper, autoRepeat);
}

void CommandParser::add(string_view name,
                        string_view params,
                        string_view help,
                        function<void(vector<string>)> action,
                        bool autoRepeat)
{
    Command command;
    command.name = name;
    command.params = params;
    command.help = help;
    command.action = action;
    command.autoRepeat = autoRepeat;
    assert(is_valid_name(name));
    assert(action);
    assert(!_groups.empty());
    vector<Command>& commands = _groups.back().commands;
    commands.push_back(std::move(command));
    std::sort(commands.begin(), commands.end(), 
        [](Command& a, Command& b) { return a.name.compare(b.name) < 0; });
}

/* Convert a hexadecimal digit to an integer. Asserts that it's in range. */
static int hex_to_int(char digit)
{
    if ('A' <= digit && digit <= 'F')
    {
        return digit - 'A' + 10;
    }
    if ('a' <= digit && digit <= 'f')
    {
        return digit - 'a' + 10;
    }
    if ('0' <= digit && digit <= '9')
    {
        return digit - '0';
    }
    assert(!"Unreachable");
}

/* Parses an escape sequence (C-style) at the beginning of line (the backslash
 * should already have been skipped). On success, true is returned and the
 * character that was encoded is stored in 'result' and `line` is advanced
 * past the escape sequence with remove_prefix(). On failure, an error message
 * is printed and false is returned. */
static bool extract_escape(string_view& line, char& result)
{
    if (line.empty())
    {
        error("Expected a C-style escape sequence after '\\'.");
        return false;
    }

    // Handle single-character escapes first
    switch (line[0])
    {
        case 'n':   result = '\n';  break;
        case 'r':   result = '\r';  break;
        case 't':   result = '\t';  break;
        case 'b':   result = '\b';  break;
        case 'f':   result = '\f';  break;
        case 'v':   result = '\v';  break;
        case '\\':  result = '\\';  break;
        case '\'':  result = '\'';  break;
        case '?':   result = '\?';  break;
        case '"':   result = '"';   break;
        default:    result = -1;    break; // sentinel value
    }
    if (result != -1)
    {
        line.remove_prefix(1);
        return true;
    }

    // Handle octal codes
    int value = 0;
    if (isdigit(line[0]))
    {
        value = line[0] - '0';
        line.remove_prefix(1);
        for (int i = 0; i < 2 && !line.empty() && isdigit(line[0]); ++i)
        {
            value = value * 8 + (line[0] - '0');
            line.remove_prefix(1);
        }
        if (value < 0 || value > 255)
        {
            error("Octal escape sequence is outside the permitted range.");
            return false;
        }
        result = (char)value;
        return true;
    }

    // Handle hex codes
    if (line[0] == 'x')
    {
        line.remove_prefix(1);
        if (line.empty() || !isxdigit(line[0]))
        {
            error("Expected hexadecimal digits after escape sequence '\\x'.");
            return false;
        }
        int i;
        for (i = 0; i < 2 && !line.empty() && isxdigit(line[0]); ++i)
        {
            value = value * 16 + hex_to_int(line[0]);
            line.remove_prefix(1);
        }
        if (i == 0)
        {
            error("Expected at least one hexadecimal digit after '\\x'.");
            return false;
        }
        assert(0 <= value && value <= 255);
        result = (char)value;
        return true;
    }

    // TODO maybe handle unicode escapes but honestly why bother
    error("Invalid escape sequence starting with '{}'.", line[0]);
    return false;
}

/* Extracts a token from line using line[0] as the delimiter. Will interpret
 * C-style escapes within the token. Advances line to where the function has
 * finished parsing with remove_prefix(). Prints an error message and returns 
 * false if invalid escapes were found, or there was no closing delimiter. */
static bool extract_string(string_view& line, string& token)
{
    assert(!line.empty());
    char delim = line[0];
    line.remove_prefix(1); // skip the delimiter
    token.clear();

    for (;;) {
        while (!line.empty() && line[0] != delim && line[0] != '\\')
        {
            token += line[0];
            line.remove_prefix(1); // advance forwards
        }
        if (line.empty())
        {
            error("Error when parsing: Unmatched delimiter: {}", delim);
            return false;
        }
        if (line[0] == delim)
        {
            line.remove_prefix(1);
            return true;
        }
        if (line[0] == '\\')
        {
            char c;
            line.remove_prefix(1); // skip the backslash
            if (!extract_escape(line, c)) // prints error
            {
                return false;
            }
            token += c;
        }
    }
}

/* Tokenises the provided line. Tokens can be separated by spaces. Interprets
 * strings formed from single and double quotes as a single token. These can
 * contain C-style escapes in them (use a double backslace to avoid that). On
 * error, an error message is printed and an empty list is returned. */
static vector<string> tokenise(string_view line)
{
    vector<string> tokens;
    for (;;) {
        // Skip whitespace
        while (!line.empty() && isspace(line[0]))
        {
            line.remove_prefix(1);
        }
        if (line.empty())
        {
            return tokens;
        }

        // handle strings
        if (line[0] == '"' || line[0] == '\'')
        {
            string token;
            if (!extract_string(line, token)) // prints error
            {
                return {};
            }
            tokens.push_back(std::move(token));
            continue;
        }

        // find the end of this token
        size_t pos = 0;
        while (pos < line.size() && !isspace(line[pos]))
        {
            pos++;
        }
        tokens.emplace_back(line.substr(0, pos));
        line.remove_prefix(pos);
    }
}

bool CommandParser::do_command(string_view thePrompt)
{
    string line;
    if (!read_line(thePrompt, line, true)) // use completer=true
    {
        return false;
    }

    if (line.empty())
    {
        if (_autoRepeatCommand.empty())
        {
            return true;
        }
        std::swap(line, _autoRepeatCommand); // repeat last command
    }
    _autoRepeatCommand.clear();

    vector<string> tokens = tokenise(line);
    debug("tokens = [{}]", join(tokens, ','));

    if (tokens.empty())
    {
        return true;
    }
    const Command* cmd = find_command(tokens[0]); // prints errors for us
    if (!cmd)
    {
        return true;
    }
    tokens.erase(tokens.begin()); // remove command name

    try 
    {
        cmd->action(std::move(tokens)); // do the thing
    }
    catch (const std::exception& e)
    {
        error("The command threw an error: {}", e.what());
    }
    
    if (cmd->autoRepeat)
    {
        _autoRepeatCommand = std::move(line);
    }
    return true;
}
