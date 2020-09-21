/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  scroll-view
 *
 *      TODO
 */
#ifndef FORKTRACE_SCROLL_VIEW_HPP
#define FORKTRACE_SCROLL_VIEW_HPP

#include <string>
#include <functional>
#include <curses.h>

/* A scrollable curses view that enables the user to scroll around the diagram
 * and inspect certain nodes. The view allows two lines of info at the top. 
 * Note that coordinates work by: (0,0) at top-left, then x increases to the
 * right and y increases downwards. */
class ScrollView 
{
public:
    /* The key constant passed to this callback is the same value returned by
     * the curses getch() method (KEY_RESIZE is filtered out). */
    using KeyCallback = std::function<void(ScrollView&, int)>;

private:
    WINDOW* _pad;
    size_t _padWidth;
    size_t _padHeight;
    size_t _cursorX; // x position of cursor (relative to the pad, not screen)
    size_t _cursorY; // y position of cursor (relative to the pad, not screen)
    bool _running; // set to false when we want to quit.
    std::string _lines[2];
    std::string _helpMessage;
    KeyCallback _keyHandler;

    /* Private functions, see source file. */
    void draw_window(bool resized = true);
    void build_image(const Window& image);
    void cleanup();

public:
    /* The constructor brings up the curses view. */
    ScrollView(const Window& window, 
               std::string_view helpMessage, 
               KeyCallback onKey);
    ~ScrollView() { cleanup(); }

    /* Allows the caller to set the messages stored at the two lines of text
     * at the top of the window. asserts that y == 0 || y == 1. */
    void set_line(std::string_view line, size_t y);

    /* Highlights a position on the provided input window. An assertion will
     * fail if the coordinates are outside the range of `window`. */
    void set_cursor(size_t x, size_t y);

    void quit() { _running = false; }   // Call from within onKeyPress handler.
    void beep();                        // Get terminal to make a beep noise.
    void update(const Window& window);  // Change the stuff being displayed.
    void run();                         // Starts drawing and does command loop
};

/* Reverse any modifications that might have been done on the terminal. This 
 * should be installed as an atexit hook and via signal handlers so that even 
 * on abrupt exits, the terminal can be reset. */
void restore_terminal();

#endif /* FORKTRACE_SCROLL_VIEW_HPP */
