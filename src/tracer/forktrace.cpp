/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  forktrace
 *
 *      TODO
 */
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <map>
#include <iostream>
#include <thread>
#include <optional>
#include <functional>
#include <atomic>

#include "forktrace.hpp"
#include "system.hpp"
#include "util.hpp"
#include "log.hpp"
#include "parse.hpp"
#include "command.hpp"
#include "tracer.hpp"
#include "process.hpp"
#include "diagram.hpp"
#include "scroll-view.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::map;
using std::shared_ptr;
using std::runtime_error;
using std::function;
using fmt::format;

/* A dummy exception object that we'll use to indicate when we want to exit
 * the command loop. Other option would be setting a flag in the command.
 * (I'd rather not modify CommandParser so that handlers returned a value.) */
struct QuitCommandLoop { };

/* TODO Explain. TODO MEMORY FENCE?? TODO volatile? TODO yeet??? */
static volatile std::atomic<bool> gDone = false;

static void signal_handler(int sig, siginfo_t* info, void* ucontext) 
{
    restore_terminal();
    // TODO info.si_pid seems bonkers??
    string msg = format("{} ({}) got {} {{info.si_pid={}}}\n",
        program_name(), getpid(), get_signal_name(sig), info->si_pid);
    write(STDERR_FILENO, msg.data(), msg.size());
    _exit(1);
}

static void register_signals() 
{
    struct sigaction sa = {0};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = signal_handler;
    sigaction(SIGHUP, &sa, 0);
    sigaction(SIGABRT, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGQUIT, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGPIPE, &sa, 0);
    // We send this to reaper_thread to cancel it.
    sa.sa_handler = [](int) { };
    sa.sa_flags = 0; // don't want SA_RESTART here
    sigaction(SIGUSR1, &sa, 0);
}

/******************************************************************************
 * REAPER AND SIGWAITER THREADS
 *****************************************************************************/

/* This is where we can assign actions to SIGINT (Ctrl+C). For this to work,
 * SIGINT must be blocked with pthread_sigmask so that it doesn't kill us. */
static void signal_thread(Tracer& tracer, sigset_t set) 
{
    int sig;
    while (sigwait(&set, &sig) == 0) // wait for the next signal
    {
        if (gDone) 
        {
            return;
        }
        tracer.nuke(); // TODO
    }
    assert(!"sigwait shouldn't fail!");
}

/* It's the caller's responsibility to close the file. This thread reads PIDs
 * of orphaned processes from the reaper (our parent) and lets the tracer know
 * about them. We are reading from the reaper proces. */
static void reaper_thread(Tracer& tracer, FILE* fromReaper) 
{
    for (;;) 
    {
        pid_t pid;
        if (fread(&pid, sizeof(pid), 1, fromReaper) != 1) 
        {
            break;
        }
        if (gDone) 
        {
            break;
        }
        tracer.notify_orphan(pid);
    }
}

/* Execs on success, exits and sends SIGHUP to the tracer on failure. (So this
 * never returns). */
static void exec_reaper(pid_t child, int pipeToTracer) 
{
    if (dup2(pipeToTracer, STDOUT_FILENO) == -1) 
    {
        error("dup2: {}", strerror_s(errno));
        close(pipeToTracer);
        goto failed;
    }
    close(pipeToTracer);

    execlp("reaper", "reaper", 0); // try from $PATH first
    execlp("./reaper", "reaper", 0); // try in current directory otherwise
    error("execlp: {}", strerror_s(errno)); // prints to std::cerr, all good

failed:
    kill(child, SIGHUP);
    waitpid(child, nullptr, 0);
    _exit(1);
}

static FILE* start_reaper() 
{
    int reaperPipe[2];
    if (pipe(reaperPipe) == -1) 
    {
        error("pipe: {}", strerror_s(errno));
        return nullptr;
    }

    // We need the close-on-exec flag so that the (read end of the) reap pipe 
    // doesn't persist (a) in the reaper process and (b) when this process
    // fork-execs tracees later on during its execution.
    int old = fcntl(reaperPipe[0], F_GETFD);
    if (fcntl(reaperPipe[0], F_SETFD, old | FD_CLOEXEC) == -1) 
    {
        error("fcntl: {}", strerror_s(errno));
        close(reaperPipe[0]);
        close(reaperPipe[1]);
        return nullptr;
    }

    pid_t child = fork();
    if (child == -1) 
    {
        error("fork: {}", strerror_s(errno));
        close(reaperPipe[0]);
        close(reaperPipe[1]);
        return nullptr;
    }
    if (child != 0) 
    {
        // we're the parent!!!!
        exec_reaper(child, reaperPipe[1]);
        /* NOTREACHED */
    }

    // we're the child!!!
    close(reaperPipe[1]);

    // We'll die and all the tracees will be killed (due the PTRACE_O_EXITKILL
    // option being used) if the reaper dies.
    if (prctl(PR_SET_PDEATHSIG, SIGHUP) == -1) 
    {
        error("prctl: {}", strerror_s(errno));
        close(reaperPipe[0]);
        return nullptr;
    }

    FILE* fromReaper = fdopen(reaperPipe[0], "r");
    if (!fromReaper) 
    {
        error("fdopen: {}", strerror_s(errno));
        close(reaperPipe[0]);
        return nullptr;
    }
    return fromReaper;
}

static void join_sigwaiter(std::thread& sigwaiter)
{
    gDone = true;

    // We send SIGINT to this process to cause the signal thread to unblock and
    // exit. Even if the signal thread isn't currently blocking, it will go 
    // onto the pending queue and be delivered when ready.
    kill(getpid(), SIGINT);
    
    sigwaiter.join();
}

static void join_reaper(std::thread& reaper, FILE* reaperPipe)
{
    gDone = true;

    // To make the reading thread exit, we set the read() file descriptor to
    // be non-blocking so that future calls to read() end immediately. We then
    // interrupt the current read() call with a signal (SA_RESTART not set).
    int fd = fileno(reaperPipe);
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        error("Couldn't cancel reaper thread: fcntl: {}", strerror_s(errno));
        return; // we'll just let the thread remain.... who cares
    }
    pthread_kill(reaper.native_handle(), SIGUSR1); // see register_signals

    reaper.join();
}

/******************************************************************************
 * COMMANDS
 *****************************************************************************/

static void do_tree(Forktrace& ft, vector<string> args)
{
    if (args.size() > 1)
    {
        throw runtime_error("Expected no more than one argument.");
    }
    if (args.empty())
    {
        if (ft.trees.empty())
        {
            std::cerr << "There are no process trees yet.\n";
        }
        for (size_t i = 0; i < ft.trees.size(); ++i)
        {
            std::cerr << colour(Colour::BOLD, format("Process tree {}:\n", i));
            ft.trees[i]->print_tree();
        }
    }
    else
    {
        size_t i = parse_number<size_t>(args[0]);
        if (i >= ft.trees.size())
        {
            throw runtime_error("Out-of-bounds process tree index.");
        }
        ft.trees[i]->print_tree();
    }
}

static void do_trees(Forktrace& ft)
{
    if (ft.trees.empty())
    {
        std::cerr << "There are no process trees yet.\n";
    }
    for (size_t i = 0; i < ft.trees.size(); ++i)
    {
        std::cerr << format("{}: {}\n", i, ft.trees[i]->to_string());
    }
}

/* Callback for do_draw() to draw to the terminal */
static void draw(const Diagram& diagram)
{
    if (!diagram.result().print(std::cout))
    {
        warning("Had to truncate the diagram. Try the scroll view instead.");
    }
}

/* Helper function for view(). Returns a string describing the currently 
 * selected event on the process diagram. */
static string get_event_info(const Event* selected) 
{
    if (!selected) 
    {
        return "";
    }
    if (!selected->location.has_value()) 
    {
        return selected->to_string();
    }
    return format("{} @ {}", 
        selected->to_string(), selected->location->to_string());
}

/* Helper function for view(). Returns a string describing the currently 
 * selected process on the process diagram. */
static string get_process_info(const Process* selected, int eventIndex) 
{
    if (!selected) 
    {
        return "";
    }
    return format("process {} {}", selected->pid(), 
        selected->command_line(eventIndex + 1)); // TODO explain
}

/* Take a key press and figure out the new position (in terms of line and lane
 * coordinates) for the cursor on the diagram. Will handle clipping at edges of
 * the diagram. Returns true if the new values of lane and line were changed
 * from what they previous were (and modifies them correspondingly). This is
 * a helper function for view(). */
static bool update_diagram_location(const Diagram& diagram, 
                                    int key, 
                                    size_t& lane, 
                                    size_t& line) 
{
    size_t newLane = lane, newLine = line;
    switch (key) 
    {
        case KEY_LEFT: 
            newLane = std::max(lane, 1UL) - 1; 
            break;
        case KEY_RIGHT: 
            newLane += 1; 
            break;
        case KEY_UP: 
            newLine = std::max(line, 1UL) - 1; 
            break;
        case KEY_DOWN: 
            newLine += 1; 
            break;
        default: 
            assert(!"Unreachable");
    }
    newLane = std::min(newLane, diagram.lane_count() - 1);
    newLine = std::min(newLine, diagram.line_count() - 1);

    if (newLane != lane || newLine != line) 
    {
        lane = newLane;
        line = newLine;
        return true;
    }
    return false;
}

/* Callback for do_draw() to draw with the ScrollView. */
static void view(const Diagram& diagram)
{
    log("Starting up the scroll-view...");
    size_t line = 0, lane = 0, x, y;
    int eventIndex;
    const Process* process = &diagram.leader();
    const Event* selected = diagram.find(lane, line, process, eventIndex);
    diagram.get_coords(lane, line, x, y); // convert to x/y coords

    auto onKeyPress = [&](ScrollView& view, int key) {
        switch (key) {
            case KEY_LEFT:
            case KEY_RIGHT:
            case KEY_UP:
            case KEY_DOWN:
                if (!update_diagram_location(diagram, key, lane, line)) 
                {
                    view.beep();
                }
                selected = diagram.find(lane, line, process, eventIndex);
                diagram.get_coords(lane, line, x, y);
                view.set_line(get_process_info(process, eventIndex), 0);
                view.set_line(get_event_info(selected), 1);
                view.set_cursor(x, y);
                break;
            case 'q':
                view.quit();
                break;
            default:
                view.beep();
                break;
        }
    };

    string help = "Arrow keys to navigate, q to quit.";
    // TODO disable logging?
    ScrollView view(diagram.result(), std::move(help), onKeyPress);
    view.set_line(get_process_info(process, eventIndex), 0);
    view.set_line(get_event_info(selected), 1);
    view.set_cursor(x, y); // make the cursor point to that location
    view.run();
}

/* Callback for do_draw() to draw with either ScrollView or to the terminal
 * depending on if the diagram can fit on the terminal screen. */
static void draw_or_view(const Diagram& diagram)
{
    size_t width, height; // ignore height
    if (get_terminal_size(width, height) && width < diagram.result().width())
    {
        log("The diagram to too big to fit, using the scroll view instead...");
        view(diagram);
    }
    else
    {
        draw(diagram);
    }
}

static void draw_tree(Forktrace& ft, 
                      size_t treeIndex, 
                      function<void(const Diagram&)> drawer)
{
    int flags = 0;
    if (ft.opts.showNonFatalSignals)
    {
        flags |= Diagram::SHOW_NON_FATAL_SIGNALS;
    }
    if (ft.opts.showExecs)
    {
        flags |= Diagram::SHOW_EXECS;
    }
    if (ft.opts.showFailedExecs)
    {
        flags |= Diagram::SHOW_FAILED_EXECS;
    }
    if (ft.opts.showSignalSends)
    {
        flags |= Diagram::SHOW_SIGNAL_SENDS;
    }
    if (ft.opts.mergeExecs)
    {
        flags |= Diagram::MERGE_EXECS;
    }
    Diagram diagram(*ft.trees.at(treeIndex).get(), ft.opts.laneWidth, flags);
    drawer(diagram);
    if (diagram.truncated())
    {
        warning("Had to truncate some lanes. Try a larger lane width.");
    }
}

static void do_draw(Forktrace& ft, 
                    vector<string> args,
                    function<void(const Diagram&)> drawer)
{
    if (args.empty())
    {
        if (ft.trees.empty())
        {
            std::cerr << "There are no process trees yet.\n";
        }
        for (size_t i = 0; i < ft.trees.size(); ++i)
        {
            std::cerr << colour(Colour::BOLD, format("Process tree {}:\n", i));
            draw_tree(ft, i, drawer);
        }
    }
    else if (args.size() > 1)
    {
        throw runtime_error("Expected no more than one argument.\n");
    }
    else
    {
        size_t i = parse_number<size_t>(args[0]);
        if (i >= ft.trees.size())
        {
            throw runtime_error("Out-of-bounds process tree index.");
        }
        draw_tree(ft, i, drawer);
    }
}

static void do_start(Forktrace& ft, vector<string> args)
{
    if (args.empty())
    {
        throw runtime_error("Expected: PROGRAM [ARGS...]");
    }
    ft.trees.push_back(ft.tracer.start(args[0], args));
}

static void do_go(Forktrace& ft)
{
    while (ft.tracer.step())
    {
        // step() will return false when there are no tracees left, however,
        // if the reaper process is disabled, then we will never be able to
        // clear the list of tracees, so we'll loosen the condition to stop
        // step()ing to be that there can be no alive tracees left (instead of
        // the stronger condition of no tracees at all).
        if (!ft.opts.reaper && !ft.tracer.tracees_alive())
        {
            return;
        }
    }
}

static void do_run(Forktrace& ft, vector<string> args)
{
    do_start(ft, std::move(args));
    do_go(ft);
}

static void do_march(Forktrace& ft)
{
    if (!ft.tracer.tracees_exist())
    {
        std::cerr << "There are no active tracees.\n";
    }
    ft.tracer.step();
}

static void do_next(Forktrace& ft)
{
    if (!ft.tracer.tracees_exist())
    {
        std::cerr << "There are no active tracees.\n";
        return;
    }
    ft.tracer.step();
    do_draw(ft, {}, draw);
}

static void register_commands(Forktrace& ft)
{
    CommandParser& parser = ft.parser;

    parser.add("quit", "", "quit " + string(program_name()),
        [] { throw QuitCommandLoop(); }
    );

    parser.start_new_group("Log & colour");

    parser.add("colour", "on|off", "enable/disable colour",
        [](string s) { set_colour_enabled(parse_bool(s)); }
    );
    parser.add("debug", "on|off", "enable/disable debug messages",
        [](string s) { set_log_category_enabled(Log::DBG, parse_bool(s)); }
    );
    parser.add("verbose", "on|off", "enable/disable extra log messages",
        [](string s) { set_log_category_enabled(Log::VERB, parse_bool(s)); }
    );
    parser.add("log", "on|off", "enable/disable general log messages",
        [](string s) { set_log_category_enabled(Log::LOG, parse_bool(s)); }
    );

    parser.start_new_group("Process tree");

    parser.add("list", "", "print a list of all tracees",
        [&] { ft.tracer.print_list(); }
    );
    parser.add("tree", "[TREE]", 
        "debug output for a process tree, or all if none specified",
        [&](vector<string> args) { do_tree(ft, std::move(args)); }
    );
    parser.add("trees", "", "print a list of all the process trees",
        [&] { do_trees(ft); }
    );
    parser.add("draw", "[TREE]", 
        "draw a process tree, or all if none specified",
        [&](vector<string> args) { 
            do_draw(ft, std::move(args), draw); 
        }
    );
    parser.add("view", "[TREE]",
        "view a process tree in a scrollable window (defaults to tree 0)",
        [&](vector<string> args) { 
            do_draw(ft, std::move(args), view); 
        }
    );

    parser.start_new_group("Tracee control");

    parser.add("start", "PROGRAM [ARGS...]", "start a tracee program",
        [&](vector<string> args) { do_start(ft, std::move(args)); }
    );
    parser.add("run", "PROGRAM [ARGS...]", 
        "equivalent to \"start\" followed by \"go\"",
        [&](vector<string> args) { do_run(ft, std::move(args)); }
    );
    parser.add("march", "", "resume all tracees until they stop again",
        [&] { do_march(ft); }, true
    );
    parser.add("next", "", "equivalent to \"march\" followed by \"draw\"",
        [&] { do_next(ft); }, true
    );
    parser.add("go", "", "resumes all tracees until until they end",
        [&] { do_go(ft); }
    );

    parser.start_new_group("Diagram config");

    parser.add("lane-width", "WIDTH", "set the diagram lane width",
        [&](string s) { ft.opts.laneWidth = parse_number<size_t>(s); }
    );
    parser.add("show-non-fatal", "yes|no", "hide or show non-fatal signals",
        [&](string s) { ft.opts.showNonFatalSignals = parse_bool(s); }
    );
    parser.add("show-execs", "yes|no", "hide or show successful execs",
        [&](string s) { ft.opts.showExecs = parse_bool(s); }
    );
    parser.add("show-bad-execs", "yes|no", "hide or show failed execs",
        [&](string s) { ft.opts.showFailedExecs = parse_bool(s); }
    );
    parser.add("show-signal-sends", "yes|no", "hide or show signal sends",
        [&](string s) { ft.opts.showSignalSends = parse_bool(s); }
    );
    parser.add("merge-execs", "yes|no", 
        "if true, merge retried execs of the same program",
        [&](string s) { ft.opts.mergeExecs = parse_bool(s); }
    );
}

/******************************************************************************
 * MAIN COMMAND LOOP, ETC.
 *****************************************************************************/

static bool confirm_quit(Forktrace& ft, bool dueToEOF)
{
    if (!ft.tracer.tracees_alive())
    {
        if (dueToEOF)
        {
            std::cerr << "EOF\n";
        }
        return true;
    }
    std::cerr << "There are still tracees alive. Quitting will kill them.\n\n";
    string line;
    if (!read_line("    Are you sure? (y/N) ", line))
    {
        std::cerr << "EOF\n";
        return true;
    }
    if (line == "y" || line == "Y")
    {
        return true;
    }
    std::cerr << '\n';
    return false;
}

static void command_line(Forktrace& ft)
{
    while (true)
    {
        try
        {
            while (ft.parser.do_command("(ft) ")) 
            { 
                ft.tracer.check_orphans(); // see header comment
            }
            if (confirm_quit(ft, true))
            {
                return;
            }
        }
        catch (const QuitCommandLoop& e)
        {
            if (confirm_quit(ft, false))
            {
                return;
            }
        }
    }
}

static bool run(Tracer& tracer, Forktrace::Options& opts, vector<string> command)
{
    vector<shared_ptr<Process>> trees; // root of each process tree
    CommandParser cmdline;

    // Bundles up references to all the state so others can access it
    Forktrace ft(opts, tracer, cmdline, trees);
    register_commands(ft);

    if (command.empty())
    {
        verbose("No command provided. Going into command line mode.");
        command_line(ft);
    }
    else
    {
        log("Starting the command: {}", join(command));
        try
        {
            assert(!command.empty());
            trees.push_back(tracer.start(command[0], command));
            do_go(ft);
            if (opts.forceScrollView)
            {
                do_draw(ft, {}, view);
            }
            else
            {
                do_draw(ft, {}, draw_or_view);
            }
        }
        catch (const std::exception& e)
        {
            error("Got error during trace: {}", e.what());
            return false;
        }
    }
    return true;
}

/* Basically the entry point to the program (called from main) after we've done
 * all of the parsing of the command-line options. If !command.empty(), then we
 * run the specified command in forktrace from start to finish. Otherwise, we
 * go into our command line mode. */
bool forktrace(vector<string> command, Forktrace::Options opts)
{
    atexit(restore_terminal);
    register_signals();

    /* Block SIGINT so it doesn't kill us (we want to sigwait it). We need to
     * do this before creating the sigwait thread and the reaper thread (via
     * start_reaper) so that all threads inherit it. We also do it before we
     * create the reaper so that SIGINT doesn't kill that either. start_tracee
     * will make sure that children don't inherit any of this for us. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    /* This will fork the reaper process as the parent. When the call is done,
     * we will be running as the child!!! (We'll have a different PID!!!). We
     * also start the reaper thread, which reads from pipe onto the queue. */
    FILE* reaperPipe;
    std::optional<std::thread> reaper;
    if (opts.reaper)
    {
        if (!(reaperPipe = start_reaper()))
        {
            error("Failed to start reaper.");
            return false;
        }
    }
    log("Hello, I'm {}", getpid());

    /* Start the reaper and sigwait threads. */
    Tracer tracer;
    if (opts.reaper)
    {
        reaper.emplace(reaper_thread, std::ref(tracer), reaperPipe);
    }
    std::thread sigwaiter(signal_thread, std::ref(tracer), set);

    bool ok = run(tracer, opts, std::move(command));

    join_sigwaiter(sigwaiter);
    if (opts.reaper)
    {
        join_reaper(reaper.value(), reaperPipe);
        fclose(reaperPipe);
    }

    return ok;
}
