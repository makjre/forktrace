#include <map>
#include <stdexcept>
#include <signal.h>
#include <sstream>
#include <iterator>
#include <readline/readline.h>
#include <readline/history.h>

#include "command.h"
#include "util.h"
#include "terminal.h"

using namespace std;

struct Command {
    bool hasArgs;
    bool autoRepeat;
    function<void(vector<string>)> action;
    string brief;
};

/* List of commands. I just love that the STL has a version of a hash-map that
 * automatically sorts itself; all the commands are in alphabetical order and
 * I didn't even have to do anything! */
static map<string, Command> commands;

/* Our EOF handler callback */
static function<bool()> onEOF;

/* The most recent command that was executed */
static string lastCommand;

void initCommands(function<bool()> eofHandler) {
    onEOF = eofHandler;
    rl_bind_key('\t', rl_complete);
    stifle_history(1000); // impose an upper limit on length of history

    // Register built-in commands
    registerCommand(
        "help",
        [] {
            for (auto& pair : commands) {
                cout << "  " << pair.first 
                    << " --- " << pair.second.brief << endl;
            }
        },
        "prints this help message"
    );
}

bool isInvalidChar(char c) {
    return isspace(c) || c == ',' || c == '"' || c == '\'';
}

void registerCommandInternal(
        string_view pName,
        bool hasArgs,
        function<void(vector<string>)> action,
        string brief,
        bool autoRepeat)
{
    assert(action);
    string name(pName);

    if (find_if(name.begin(), name.end(), isInvalidChar) != name.end()) {
        throw invalid_argument("name");
    }

    for (auto& pair : commands) {
        if (pair.first.starts_with(name)) { // also true if key==name
            throw invalid_argument("name");
        }
    }

    Command command;
    command.hasArgs = hasArgs;
    command.autoRepeat = autoRepeat;
    command.action = action;
    command.brief = move(brief);
    commands.emplace(name, move(command));
}

void registerCommandWithArgs(
        string_view name,
        function<void(vector<string>)> action,
        string brief,
        bool autoRepeat)
{
    registerCommandInternal(name, true, action, brief, autoRepeat);
}

void registerCommand(
        string_view name,
        function<void()> action,
        string brief,
        bool autoRepeat)
{
    registerCommandInternal(name, false, bind(action), brief, autoRepeat);
}

Command* findCommand(const string& name) {
    vector<decltype(commands)::iterator> partialMatches;

    for (auto it = commands.begin(); it != commands.end(); ++it) {
        if (it->first.starts_with(name)) {
            if (it->first.length() == name.length()) {
                // We've found a perfect match
                return &it->second;
            }

            partialMatches.push_back(it);
        }
    }

    if (partialMatches.empty()) {
        cout << "Could not find any commands matching \"" 
            << name << "\". Try \"help\"." << endl;
        return nullptr;
    }

    if (partialMatches.size() > 1) {
        cout << "Ambiguous command. Options are:";
        for (auto it : partialMatches) {
            cout << ' ' << it->first;
        }
        cout << endl;
        return nullptr;
    }

    return &partialMatches.front()->second;
}

vector<string> tokenise(const string& input) {
    vector<string> args;
    const char* ch = input.c_str();
    char delim;
    // TODO C escapes??

    for (;;) {
        while (*ch != '\0' && isspace(*ch)) { 
            ch++; 
        }
        if (*ch == '\0') {
            return args;
        }

        delim = 0;
        if (*ch == '"' || *ch == '\'') {
            delim = *ch;
            ch++;
        }
        const char* arg = ch;

        while (*ch != '\0' && *ch != delim) {
            if (delim == 0 && isspace(*ch)) {
                break;
            }
            ch++;
        }

        args.emplace_back(arg, ch);
    
        if (delim != 0) {
            if (*ch != delim) {
                throw runtime_error("Couldn't find closing delimiter.");
            }
        }

        if (*ch == '\0') {
            return args;
        }
        ch++;
    }
}

bool prompt(string_view str, string& result) {
    struct sigaction sa = {0}, old;
    sa.sa_flags = 0;
    sa.sa_handler = [](int) { };
    sigaction(SIGINT, &sa, &old);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

    string prompt(str);
    char* input = readline(prompt.c_str()); // grrr c strings

    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    sigaction(SIGINT, &old, nullptr);

    if (!input) {
        return false;
    }

    // Add input to readline history
    add_history(input);

    result.assign(input);
    free(input);
    return true;
}

void commandLoop(string_view thePrompt) {
    for (;;) {
        string line;
        if (!prompt(colourise(thePrompt, Colour::BOLD), line)) {
            if (!onEOF || !onEOF()) { // callback
                return;
            }
        }

        if (line.empty()) {
            if (lastCommand.empty()) {
                continue;
            }
            swap(line, lastCommand);
        } else {
            strip(line);
        }
        lastCommand.clear();

        auto firstSpace = find_if(line.begin(), line.end(), 
                [](char c) { return isspace(c); });

        string cmdStr(line.begin(), firstSpace);
        string params(firstSpace, line.end());
        strip(params);

        Command* cmd = findCommand(cmdStr); // prints error message for us
        if (!cmd) {
            continue;
        }
        
        if (!params.empty() && !cmd->hasArgs) {
            cout << "This command does not accept any arguments." << endl;
            continue;
        }

        assert(cmd->action);
        try {
            cmd->action(tokenise(params));
        } catch (const QuitCommandLoop& e) {
            return;
        } catch (const exception& e) {
            cout << colourise("error: ", Colour::RED_BOLD) << e.what() << endl;
        }

        if (cmd->autoRepeat) {
            lastCommand = move(line); 
        }
    }
}

bool parseBool(string_view input) {
    string str(input);
    transform(str.begin(), str.end(), str.begin(), ::tolower);

    if (str == "yes" || str == "1" || str == "on" || str == "enabled" 
            || str == "enable" || str == "true") {
        return true;
    }

    if (str == "no" || str == "0" || str == "off" || str == "disabled"
            || str == "disable" || str == "false") {
        return false;
    }

    throw runtime_error("Invalid boolean parameter.");
}
