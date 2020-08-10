#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include "tracer.h"
#include "system.h"
#include "process.h"
#include "util.h"
#include "diagram.h"
#include "terminal.h"
#include "command.h"

using namespace std;

/* To cleanly find out when a process is orphaned, we exec a reaper process
 * (and run the tracer as the child instead) that sits above us and sends the
 * pids of orphans that it has collected through a pipe. We'll then create a
 * thread to read those pids from the pipe onto this queue. We'll tell the
 * tracer how it can access these orphans via a callback function.
 */
static queue<pid_t> orphans;
static mutex orphansLock;

/* TODO Explain. TODO MEMORY FENCE?? TODO volatile? */
static volatile atomic<bool> done = false;

/* Configuration options that we give to Diagrams */
static unsigned laneWidth = 5;
static bool hideNonFatalSignals = true;

/* Configuration options for whether we print diagram debug info */
static bool printDiagramDebug = false;

/* Command line prompt */
static const char* PROMPT = ">> ";

void handler(int sig, siginfo_t* info, void* ucontext) {
    restoreTerminal();

    char buf[100];
    string_view name = getSignalName(sig);
    snprintf(buf, 100, "tracer (%d) got %.*s(%d) {info.si_pid=%d}\n",
            (int)getpid(), (int)name.length(), name.data(), sig, 
            (int)info->si_pid);
    write(STDERR_FILENO, buf, strlen(buf));

    _exit(1);
}

void registerSignals() {
    struct sigaction sa = {0};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = handler;
    sigaction(SIGHUP, &sa, 0);
    sigaction(SIGABRT, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGQUIT, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGPIPE, &sa, 0);

    // use this to interrupt blocking system calls
    sa.sa_handler = [](int) { };
    sigaction(SIGUSR1, &sa, 0);
}

void signalThread(Tracer& tracer, sigset_t set) {
    int sig;
    while (sigwait(&set, &sig) == 0) {
        if (done) {
            return;
        }
        cout << "Halting" << endl;
        tracer.halt();
    }
    throw runtime_error("sigwait failed...?!?! What do I do now??");
}

/* Check if there's an orphan's pid available on the queue and return it by
 * reference if there is (and return true). Otherwise return false. Intended
 * to be used by the Tracer (we'll give this function to it as a callback). */
bool popOrphan(pid_t& pid) {
    scoped_lock<mutex> guard(orphansLock);
    if (orphans.empty()) {
        return false;
    }
    pid = orphans.front();
    orphans.pop();
    return true;
}

/* It's the caller's responsibility to close the file. */
void reaperThread(FILE* fromReaper) {
    for (;;) {
        pid_t pid;
        if (fread(&pid, sizeof(pid), 1, fromReaper) != 1) {
            break;
        }
        if (done) {
            break;
        }
        scoped_lock<mutex> guard(orphansLock);
        orphans.push(pid);
    }
}

/* Execs on success, exits and sends SIGHUP to the tracer on failure. (So this
 * never returns). */
void execReaper(pid_t child, int pipeToTracer) {
    try {
        if (dup2(pipeToTracer, STDOUT_FILENO) == -1) {
            int e = errno;
            close(pipeToTracer);
            throw system_error(e, generic_category(), "dup2");
        }
        close(pipeToTracer);

        execlp("reaper", "reaper", 0);
        throw system_error(errno, generic_category(), "execl");
        /* NOTREACHED */
    } catch (const exception& e) {
        kill(child, SIGHUP);
        waitpid(child, nullptr, 0);
        cerr << "Failed to exec reaper:" << endl
            << "  what():  " << e.what() << endl;
        _exit(1);
        /* NOTREACHED */
    }
}

FILE* startReaper() {
    int reapPipe[2];
    if (pipe(reapPipe) == -1) {
        throw system_error(errno, generic_category(), "pipe");
    }
    // We need the close-on-exec flag so that the (read end of the) reap pipe 
    // doesn't persist (a) in the reaper process and (b) when this process
    // fork-execs tracees later on during its execution.
    if (fcntl(reapPipe[0], F_SETFD, FD_CLOEXEC) == -1) {
        int e = errno;
        close(reapPipe[0]);
        close(reapPipe[1]);
        throw system_error(e, generic_category(), "fcntl");
    }

    pid_t child = fork();
    if (child == -1) {
        int e = errno;
        close(reapPipe[0]);
        close(reapPipe[1]);
        throw system_error(e, generic_category(), "fork");
    }
    if (child != 0) {
        // we're the parent
        execReaper(child, reapPipe[1]);
        /* NOTREACHED */
    }

    // we're the child
    close(reapPipe[1]);

    // not super essential - just for convenience
    if (prctl(PR_SET_PDEATHSIG, SIGHUP) == -1) {
        int e = errno;
        close(reapPipe[0]);
        throw system_error(e, generic_category(), "prctl");
    }

    FILE* fromReaper = fdopen(reapPipe[0], "r");
    if (!fromReaper) {
        int e = errno;
        close(reapPipe[0]);
        throw system_error(e, generic_category(), "fdopen");
    }
    return fromReaper;
}

void joinThreads(thread& reapThread, FILE* reaperPipe, thread& sigThread) {
    // set the global flag to true so the threads know to exit
    done = true;

    // However the threads are probably blocking on read() and sigwait()
    // respectively, so we need to cause those to unblock. We send SIGINT
    // to this process to cause the signal thread to unblock and exit. Even
    // if the signal thread isn't currently blocking, it doesn't matter, since
    // the signal will go onto the pending queue and be delivered when ready.
    kill(getpid(), SIGINT);

    // To make the reading thread exit, we set the read() file descriptor to
    // be non-blocking so that future calls to read() end immediately. We then
    // interrupt the current read() call with a signal (SA_RESTART not set).
    int flags = fcntl(fileno(reaperPipe), F_GETFL);
    if (flags == -1) {
        throw system_error(errno, generic_category(), "fcntl");
    }
    if (fcntl(fileno(reaperPipe), F_SETFL, flags | O_NONBLOCK) == -1) {
        throw system_error(errno, generic_category(), "fcntl");
    }
    pthread_kill(reapThread.native_handle(), SIGUSR1);

    // We can now join them without waiting forever ;-)
    sigThread.join();
    reapThread.join();
}

vector<string> getArgs(int argc, const char** argv) {
    vector<string> args;
    args.reserve(argc);
    for (unsigned i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

void draw(const Process& tree) {
    Diagram::Options opts(laneWidth, true, false, hideNonFatalSignals, false);
    Diagram diagram(tree, opts);

    if (printDiagramDebug) {
        diagram.print();
    }

    if (!diagram.result().print()) {
        cout << Colour::RED
            << "The diagram could not fit within the screen width."
            << Colour::RESET << endl;
        cout << Colour::BOLD << "note: " << Colour::RESET
            << "Consider trying the scrollable TUI view instead." << endl;
    }

    if (diagram.truncated()) {
        cout << Colour::RED 
            << "The diagram's lane width was too small." 
            << Colour::RESET << endl;
    }
}

bool tryClear(Tracer& tracer, shared_ptr<Process>& tree) {
    if (!tracer.traceesAlive() && !tree) {
        return true;
    }

    if (tracer.traceesAlive()) {
        cout << tracer.traceeCount() << " tracee(s) will be killed." << endl;
    }
    if (tree) {
        cout << "The current process tree will be erased." << endl;
    }

    cout << endl << Indent(1) << flush;
    string input;
    if (!prompt("Are you sure? (y/N) ", input)) {
        cout << endl;
        return false;
    }
    cout << endl;

    if (input == "y") {
        tree = nullptr;
        tracer.nuke();
    }

    return true;
}

void tryStartTracee(Tracer& tracer, shared_ptr<Process>& tree,
        const vector<string>& args) 
{
    if (args.empty()) {
        cout << "You must give me a program to execute." << endl;
        return;
    }
    if (tracer.traceesAlive() || tree) {
        cout << "You must end the current session first." << endl;
        if (!tryClear(tracer, tree)) {
            return;
        }
    }
    tree = tracer.start(args);
}

void tryMarch(Tracer& tracer) {
    if (!tracer.traceesAlive()) {
        cout << "There are no active tracees." << endl;
    }
    tracer.march();
}

void tryGo(Tracer& tracer) {
    if (!tracer.traceesAlive()) {
        cout << "There are no active tracees." << endl;
    } else {
        tracer.go();
    }
}

void printKey() {
    cout << Colour::BOLD << "~~~~~~~~~~~~~ KEY ~~~~~~~~~~~~~" 
        << Colour::RESET << endl;

    cout << EXEC_COLOUR << " E " << Colour::RESET 
        << " successful exec" << endl;

    cout << BAD_EXEC_COLOUR << " E " << Colour::RESET 
        << " failed exec" << endl;

    cout << SIGNAL_COLOUR << " x " << Colour::RESET
        << " received a signal" << endl;

    cout << SIGNAL_SEND_COLOUR << " x " << Colour::RESET
        << " called kill/tgkill/tkill" << endl;

    cout << Colour::BOLD << "i-- waited for specific child"
        << Colour::RESET << endl;

    cout << Colour::BOLD << "w-- waited for any child"
        << Colour::RESET << endl;

    cout << Colour::BOLD << "g-- waited for child in group"
        << Colour::RESET << endl;

    cout << BAD_WAIT_COLOUR << " w " << Colour::RESET
        << " failed wait (i, w, or g)" << endl;

    cout << EXITED_COLOUR << "--- reaped, exited"
        << Colour::RESET << endl;

    cout << '(' << EXITED_COLOUR << 'x' << Colour::RESET << ") "
        << EXITED_COLOUR << "orphaned, exited" << Colour::RESET << endl;

    cout << KILLED_COLOUR << "~~~ reaped, signaled" 
        << Colour::RESET << endl;

    cout << '[' << KILLED_COLOUR << 'x' << Colour::RESET << "] "
        << KILLED_COLOUR << "orphaned, signaled" << Colour::RESET << endl;

    cout << Colour::BOLD << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"
        << Colour::RESET << endl;
}

bool tryQuit(const Tracer& tracer, bool dueToEOF) {
    if (!tracer.traceesAlive()) {
        if (dueToEOF) {
            cout << "EOF" << endl;
        }
        return true; 
    }

    cout << "Quitting will cause all tracees to be killed." << endl;
    cout << endl << Indent(1) << flush;
    
    string result;
    if (!prompt("Are you sure? (y/N) ", result)) {
        cout << "EOF" << endl;
        return true;
    }
    cout << endl;

    if (result == "y") {
        return true;
    }

    return false;
}

void marchAndDraw(Tracer& tracer, const Process& tree) {
    bool aliveBefore = tracer.traceesAlive();
    tryMarch(tracer);
    if (aliveBefore) {
        draw(tree);
    }
}

void quitCommand(Tracer& tracer) {
    if (tryQuit(tracer, false)) {
        throw QuitCommandLoop(); // command.cpp will intercept this
    }
}

void killCommand(Tracer& tracer, const vector<string>& args) {
    if (args.empty()) {
        tracer.killAll();
    } else if (args.size() == 1) {
        if (kill(parseNumber<pid_t>(args[0]), SIGKILL) == -1) {
            throw system_error(errno, generic_category(), "kill");
        }
    } else {
        throw runtime_error("usage: kill [pid]");
    }
}

void setLaneWidth(unsigned width) {
    if (width < 2) {
        throw runtime_error("The lane width must be at least 2.");
    }
    laneWidth = width;
}

const string& getSingleArg(const vector<string>& args) {
    if (args.size() != 1) {
        throw runtime_error("This command expects 1 argument.");
    }
    return args[0];
}

const Process& getTree(shared_ptr<Process>& tree) {
    if (!tree) {
        throw runtime_error("The process tree is empty.");
    }
    return *tree.get();
}

shared_ptr<Process>& getTreePtr(shared_ptr<Process>& tree) {
    if (!tree) {
        throw runtime_error("The process tree is empty.");
    }
    return tree;
}

void restart(Tracer& tracer, shared_ptr<Process>& tree) {
    assert(tree->getEventCount() > 0);
    auto exec = dynamic_cast<const ExecEvent*>(&tree->getEvent(0));
    assert(exec && exec->succeeded());
    vector<string> args = exec->args;
    tracer.nuke();
    tree = tracer.start(args);
}

string getEventLine(const Event* selected) {
    if (!selected) {
        return "";
    }
    if (!selected->loc) {
        return selected->toString();
    }
    ostringstream oss;
    oss << selected->toString() << " @ " << selected->loc->toString();
    return oss.str();
}

string getProcessLine(const Process* selected, int eventIndex) {
    if (!selected) {
        return "";
    }
    ostringstream oss;
    oss << "process " << selected->pid() << ' ' 
        << selected->commandLine(eventIndex + 1); // TODO explain
    return oss.str();
}

bool updateDiagramLocation(const Diagram& diagram, int key, size_t& lane, 
        size_t& line) 
{
    size_t newLane = lane, newLine = line;
    switch (key) {
        case KEY_LEFT: 
            newLane = max(lane, 1UL) - 1; 
            break;
        case KEY_RIGHT: 
            newLane += 1; 
            break;
        case KEY_UP: 
            newLine = max(line, 1UL) - 1; 
            break;
        case KEY_DOWN: 
            newLine += 1; 
            break;
        default: 
            assert(!"Unreachable");
    }
    newLane = min(newLane, diagram.laneCount() - 1);
    newLine = min(newLine, diagram.lineCount() - 1);

    if (newLane != lane || newLine != line) {
        lane = newLane;
        line = newLine;
        return true;
    }
    return false;
}

void doScrollView(Tracer& tracer, shared_ptr<Process>& tree) {
    Diagram::Options opts(laneWidth, true, false, hideNonFatalSignals, false);
    // We'll destroy and update this diagram as we need to
    auto diagram = make_unique<Diagram>(*tree.get(), opts);

    if (diagram->truncated()) {
        cout << Colour::RED 
            << "The diagram's lane width was too small." 
            << Colour::RESET << endl;

        string dummy; // current mood
        if (!prompt("Press ENTER to continue...", dummy)) {
            return;
        }
    }

    assert(tree->getEventCount() > 0);
    size_t line = 0, lane = 0, x, y;
    int eventIndex;
    const Process* process = tree.get();
    const Event* selected = diagram->find(lane, line, process, eventIndex);
    diagram->getCoords(lane, line, x, y); // convert to x/y coords

    auto onKeyPress = [&](ScrollView& view, int key) {
        switch (key) {
            case 'n':
                if (!tracer.traceesAlive()) {
                    view.beep();
                }
                tracer.march();
                diagram = make_unique<Diagram>(*tree.get(), opts);
                view.update(diagram->result());
                lane = (process ? diagram->locate(*process) : lane);
                selected = diagram->find(lane, line, process, eventIndex);
                diagram->getCoords(lane, line, x, y);
                view.setCursor(x, y);
                break;
            case 'r':
            case 'a':
                restart(tracer, tree);
                if (key == 'a') tracer.go();
                diagram = make_unique<Diagram>(*tree.get(), opts);
                view.update(diagram->result());
                line = lane = 0;
                process = tree.get();
                selected = diagram->find(lane, line, process, eventIndex);
                diagram->getCoords(lane, line, x, y);
                view.setLine(getProcessLine(process, eventIndex), 0);
                view.setLine(getEventLine(selected), 1);
                view.setCursor(x, y); // make the cursor point to that location
                break;
            case KEY_LEFT:
            case KEY_RIGHT:
            case KEY_UP:
            case KEY_DOWN:
                if (!updateDiagramLocation(*diagram.get(), key, lane, line)) {
                    view.beep();
                }
                selected = diagram->find(lane, line, process, eventIndex);
                diagram->getCoords(lane, line, x, y);
                view.setLine(getProcessLine(process, eventIndex), 0);
                view.setLine(getEventLine(selected), 1);
                view.setCursor(x, y);
                break;
            case 'q':
                view.quit();
                break;
            default:
                view.beep();
                break;
        }
    };

    string help = "Arrow keys to navigate, "
        "q to quit, n to step, "
        "r to restart, a to re-run";
    setLogEnabled(false);
    ScrollView view(diagram->result(), move(help), onKeyPress, 5);
    view.setLine(getProcessLine(process, eventIndex), 0);
    view.setLine(getEventLine(selected), 1);
    view.setCursor(x, y); // make the cursor point to that location
    view.run();
    setLogEnabled(true);
}

void doFastDiagram(Tracer& tracer, shared_ptr<Process>& tree,
        const char** childArgs) 
{
    try {
        /* If we've been given a program then load it up for them and run it
         * until everything dies or we are told to stop. */
        tree = tracer.start(childArgs);
        tracer.go();

        /* If there's still tracees alive, then let the user take control. */
        if (tracer.traceesAlive()) {
            commandLoop(PROMPT);
        } else {
            hideNonFatalSignals = true;
            printKey();
            draw(*tree.get());
        }
    } catch (const exception& e) {
        cout << Colour::RED_BOLD << "error: " << Colour::RESET 
            << e.what() << endl;
    }
}

void registerCommands(Tracer& tracer, shared_ptr<Process>& tree) {
    registerCommand(
        "quit",
        [&] { quitCommand(tracer); },
        "exits this program (asks for confirmation if tracees are alive)"
    );
    registerCommandWithArgs(
        "start",
        [&](vector<string> args) { tryStartTracee(tracer, tree, args); },
        "starts a tracee from its command line arguments"
    );
    registerCommand(
        "tree",
        [&] { getTree(tree).printTree(); },
        "print the current process tree"
    );
    registerCommand(
        "list",
        [&] { tracer.printList(); },
        "print a list of active tracees"
    );
    registerCommand(
        "draw",
        [&] { draw(getTree(tree)); },
        "prints the diagram for the process tree to stdout"
    );
    registerCommand(
        "next",
        [&] { marchAndDraw(tracer, getTree(tree)); },
        "equivalent to \"march\" followed by \"draw\"",
        true
    );
    registerCommand(
        "go",
        [&] { tryGo(tracer); },
        "continues all tracees until they are dead or halted"
    );
    registerCommandWithArgs(
        "run",
        [&](vector<string> args) {
            tryStartTracee(tracer, tree, args);
            tracer.go();
        },
        "same as \"start program [args...]\" followed by \"go\""
    );
    registerCommand(
        "march",
        [&] { tryMarch(tracer); },
        "resume all tracees, then wait for them all to stop",
        true
    );
    registerCommandWithArgs(
        "step",
        [&](vector<string> args) { 
            tracer.step(parseNumber<pid_t>(getSingleArg(args)));
        },
        "resumes specified pid, then waits for it to stop",
        true
    );
    registerCommandWithArgs(
        "kill",
        [&](vector<string> args) { 
            killCommand(tracer, args);
        },
        "kill specified pid with SIGKILL, or all tracees if none specified"
    );
    registerCommand(
        "clear",
        [&] { tryClear(tracer, tree); },
        "SIGKILL and erase all the tracees and clear the process tree"
    );
    registerCommand(
        "key",
        [] { printKey(); },
        "print the key for the diagram"
    );
    registerCommandWithArgs(
        "view",
        [&](vector<string> args) { 
            doScrollView(tracer, getTreePtr(tree)); 
        },
        "displays the diagram of the process tree in a scrollable window"
    );
    registerCommandWithArgs(
        "colour",
        [](vector<string> args) {
            setColourEnabled(parseBool(getSingleArg(args)));
        },
        "globally enable or disable colours"
    );
    registerCommandWithArgs(
        "lane-width",
        [](vector<string> args) {
            setLaneWidth(parseNumber<unsigned>(getSingleArg(args)));  
        },
        "set the width of each lane of the diagram"
    );
    registerCommandWithArgs(
        "hide-non-fatal",
        [&](vector<string> args) {
            hideNonFatalSignals = parseBool(getSingleArg(args));
        },
        "enable/disable showing non-fatal signals on the diagram"
    );
    registerCommandWithArgs(
        "merge-execs",
        [](vector<string> args) { 
            setExecMergingEnabled(parseBool(getSingleArg(args))); 
        },
        "enable/disable merging of consecutive bad execs of the same file"
    );
    registerCommandWithArgs(
        "debug",
        [](vector<string> args) {
            setDebugLogLevel(parseNumber<unsigned>(getSingleArg(args)));
        },
        "set debug log level to: 0=none, 1=enabled, 2=verbose"
    );
}

int main(int argc, const char** argv) {
    atexit(restoreTerminal);
    registerSignals();

    /* Block SIGINT so it doesn't kill us (we want to sigwait it). We need to
     * do this before creating the sigwait thread and the reaper thread (via
     * startReaper) so that all threads inherit it. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    /* This will fork the reaper process as the parent. When the call is done,
     * we will be running as the child!!! (We'll have a different PID!!!) This
     * function throws on failure, so it doesn't return NULL. */
    FILE* reaperPipe = startReaper();
    cout << "Hello, I'm " << getpid() << endl;
    /* Start the reaper thread, which reads from pipe onto the queue. */
    thread reapThread(&reaperThread, reaperPipe);

    /* Create the tracer and tell it how it can find out about orphans from
     * us by giving it a callback function. */
    Tracer tracer(getArgs(argc, argv), popOrphan);

    /* Start our waiting thread for SIGINT. */
    thread sigThread(signalThread, ref(tracer), set);

    /* The root of our current process tree. */
    shared_ptr<Process> tree;

    initCommands([&]{ return !tryQuit(tracer, true); }); // EOF handler
    registerCommands(tracer, tree);
    if (argc > 1) {
        doFastDiagram(tracer, tree, &argv[1]);
    } else {
        commandLoop(PROMPT);
    }

    joinThreads(reapThread, reaperPipe, sigThread); 
    fclose(reaperPipe);
    return 0;
}
