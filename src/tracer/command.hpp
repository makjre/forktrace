/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  command
 *
 *      TODO
 */
#ifndef FORKTRACE_COMMAND_HPP
#define FORKTRACE_COMMAND_HPP

#include <map>
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <optional>

/* Uses GNU readline to read a line of input from the user with the provided
 * prompt. Returns false on EOF, otherwise the line is stored in `line`. If
 * complete is true, then readline's default TAB filename completion will be 
 * enabled. Non-empty lines will be added to readline's history buffer. Will
 * strip the line of trailing and leading whitespace. */
bool read_line(std::string_view prompt, 
               std::string& line, 
               bool complete = false);

/* Uses GNU readline internally which has global state. */
class CommandParser 
{
private:
    struct Command
    {
        std::string params; // Description of the parameters (maybe empty).
        std::string help; // No line breaks needed. We'll wrap it ourselves.
        std::function<void(std::vector<std::string>)> action;
        bool autoRepeat; // If true, pressing enter repeats the command.
    };

    std::map<std::string, Command> _commands;

    /* If the previous command had autoRepeat=true, then we'll store that line
     * here so that we know to repeat it if we get an empty line afterwards. */
    std::string _autoRepeatCommand;

    /* Searches for a command that using the following procedure:
     *
     *      (1) If 'prefix' matches a command name exactly, pick that.
     *      (2) If a single command name begins with 'prefix', pick that.
     *
     * Otherwise, an error is printed and null is returned. */
    const Command* find_command(std::string_view prefix) const;

    /* The internal handler for the built-in help command */
    void help_handler(std::vector<std::string> args) const;
    
public:
    CommandParser();
    
    /* Register a command that takes no arguments. */
    void add(std::string_view name,
             std::string_view params,
             std::string_view help,
             std::function<void()> action,
             bool autoRepeat = false);

    /* Register a command that takes one argument. */
    void add(std::string_view name,
             std::string_view params,
             std::string_view help,
             std::function<void(std::string)> action,
             bool autoRepeat = false);

    /* Register a command that takes any number of arguments */
    void add(std::string_view name,
             std::string_view params,
             std::string_view help,
             std::function<void(std::vector<std::string>)> action,
             bool autoRepeat = false);

    /* Requests a command from the user and performs any actions described by
     * it, or prints errors if the command was invalid. Returns false on EOF.
     * Catches any std::exception's thrown by the command and prints them. */
    bool do_command(std::string_view prompt);
};

#endif /* FORKTRACE_COMMAND_HPP */
