#ifndef COMMAND_H
#define COMMAND_H

#include <cassert>
#include <string>
#include <vector>
#include <functional>
#include <climits>
#include <system_error>
#include <charconv>

/* If a command throws this, then the command loop is terminated. If I used the
 * return value of command callbacks to indicate this, all my lambdas would be
 * much less brief :-(. */
class QuitCommandLoop {
public:
    QuitCommandLoop() { }
};

/* Registers a handler that is called when EOF is read while prompting for a
 * command. If the handler returns true, then the user is reprompted. If the
 * handler returns false, then the command loop returns. If no EOF handler is 
 * specified, then the command loop will default to returning on EOF. This also
 * initialises GNU readline. Also registers the built-in command 'help'. */
void initCommands(std::function<bool()> onEOF = nullptr);

/* Add a command to the list of registered commands that will be interpreted
 * by 'executeCommand'. Commands with the name 'get', 'set' and 'help' are
 * reserved. A std::invalid_argument exception will be thrown if the `name` is
 * already in the list, or also if `name` has any whitespace/quotes/commas. */

void registerCommand(
        std::string_view name,
        std::function<void()> action,
        std::string brief,
        bool autoRepeat = false);

void registerCommandWithArgs(
        std::string_view name,
        std::function<void(std::vector<std::string>)> action,
        std::string brief,
        bool autoRepeat = false);

/* Prints the specified prompt and reads a line of input using GNU readline.
 * Returns false if an EOF marker was reached. This is done for you by the
 * commandLoop function - it is only exposed if you want to use it for your
 * own nefarious purposes. */
bool prompt(std::string_view prompt, std::string& result);

/* Returns once EOF is reached and either the EOF handler does not exist or 
 * the EOF handler returns false. Will also return if a QuitCommandLoop object
 * is thrown by a command callback. */
void commandLoop(std::string_view prompt);

/* Parses a boolean argument and throws an exception if the argument is not
 * valid. Accepts standard words like enabled, disabled, yes, no, true, false,
 * 0, 1. */
bool parseBool(std::string_view input);

/* Helper function to parse arbitrary integer argments. The entire string must
 * be a valid integer, otherwise an exception will be thrown. The exception is
 * a system_error, which isn't entirely related to the type of error, but it's
 * just an easy way to shove the error code into an exception with a suitable
 * what() string. */
template <class T>
T parseNumber(std::string_view input) {
    T value;
    const auto result = std::from_chars(input.data(),
            input.data() + input.size(), value);
    if (result.ptr == input.data()) {
        throw std::runtime_error("Not a valid number.");
    }
    if (result.ptr != input.data() + input.size()) {
        throw std::runtime_error("Number has trailing characters.");
    }
    if (result.ec != std::errc()) {
        throw std::system_error(std::make_error_code(result.ec));
    }
    return value;
}

#endif /* COMMAND_H */
