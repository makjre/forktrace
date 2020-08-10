#ifndef TERMINAL_H
#define TERMINAL_H

#include <curses.h>
#include <string>
#include <functional>
#include <cstdint>

enum class Colour : uint8_t {
    BLACK,
    GREY,
    YELLOW,
    BLUE,
    BLUE_BOLD,
    GREEN_BOLD,
    RED,
    RED_BOLD,
    MAGENTA,
    WHITE,
    BOLD,
    RESET = WHITE,
};

/* Just translates the colour into its ANSI escape sequence. If colour is
 * disabled this does nothing. */
std::ostream& operator<<(std::ostream& os, Colour c);

/* Encloses the string in ANSI escape sequences to give it the desired colour
 * (and reset the colour at the end). Does nothing if colour is disabled. */
std::string colourise(std::string_view s, Colour c);

/* Enable or disable colours globally. */
void setColourEnabled(bool enable);

/* Just represents a grid of characters that we can draw to with colours of
 * a terminal. This can be printed out to the console or converted to a curses
 * pad for use with a TUI. I was going to just represent this directly with a
 * curses pad, but those can only exist while curses is set up, so it's easier
 * to just do it myself. */
class Window {
public:
    struct __attribute__((packed)) Cell {
        Colour colour;
        char ch;

        Cell() : colour(Colour::WHITE), ch(' ') { }
        Cell(Colour c, char ch) : colour(c), ch(ch) { }
    };

private:
    Colour _current;
    Colour _default;
    size_t _width;
    size_t _height;
    Cell* _buf; // stored in rows

    Cell& at(size_t x, size_t y) { return _buf[y * _width + x]; }

public:
    Window(size_t width, size_t height, Colour defaultColour = Colour::WHITE);
    ~Window();

    Colour setColour(Colour newColour);
    Colour resetColour() { return setColour(_default); }
    void drawChar(size_t x, size_t y, char ch, size_t count = 1);
    void drawString(size_t x, size_t y, std::string_view str);
    Cell getCell(size_t x, size_t y) const { return _buf[y * _width + x]; }
    size_t width() const { return _width; }
    size_t height() const { return _height; }

    /* Prints this window to stdout. If colour==true, then this will use ANSI 
     * escape sequences to achieve the desired colours. Before printing, the 
     * function will query the current width of the window and truncate the 
     * output to fit within it. Returns false if had to be truncated. */
    bool print(bool colour = true) const;
};

/* A scrollable curses view that enables the user to scroll around the diagram
 * and inspect certain nodes. The view allows two lines of info at the top. */
class ScrollView {
public:
    /* The key constant passed to this callback is the same value returned by
     * the curses getch() method (KEY_RESIZE is filtered out). */
    using KeyCallback = std::function<void(ScrollView&, int)>;

private:
    WINDOW* _pad;
    size_t _padWidth;
    size_t _padHeight;
    size_t _padX; // x offset of pad onto screen
    size_t _padY; // y offset of pad onto screen
    size_t _cursorX; // x position of cursor
    size_t _cursorY; // y position of cursor
    size_t _scrollMargin;
    bool _running; // set to false when we want to quit.
    std::string _lines[2];
    std::string _helpMessage;
    KeyCallback _keyHandler;

    void drawWindow(bool resized = true);
    void buildImage(const Window& image);
    void cleanup();

public:
    /* scrollMargin describes how close to the edge of the terminal window the
     * cursor can get before forcing the view to recenter. If the margin is
     * larger than the relevant dimension of the window, then the window is
     * just centered along that axis. */ 
    ScrollView(const Window& window, std::string helpMessage, 
            KeyCallback onKey, size_t scrollMargin);
    ~ScrollView() { cleanup(); }

    /* Supports line 0 and line 1. Otherwise an assertion fails. */
    void setLine(std::string_view line, size_t y);

    /* Highlights a position on the provided input window. An assertion will
     * fail if the coordinates are outside the range of `window`. */
    void setCursor(size_t x, size_t y);

    void quit() { _running = false; }
    void beep();
    void update(const Window& window);
    void run();
};

/* Reverse any modifications that might have been done on the terminal. This 
 * should be installed as an atexit hook and via signal handlers so that even 
 * on abrupt exits, the terminal can be reset. */
void restoreTerminal();

#endif /* TERMINAL_H */
