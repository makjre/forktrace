/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  terminal
 *
 *      TODO
 */
#include <unistd.h>
#include <sys/ioctl.h>
#include <cassert>
#include <limits>
#include <atomic>
#include <fmt/core.h>
#include <fmt/color.h>

#include "text-wrap.hpp"
#include "terminal.hpp"
#include "util.hpp"

using std::string;
using std::string_view;
using std::runtime_error;

/* Affects how colour() behaves. */
static std::atomic<bool> gColourEnabled = true;

void set_colour_enabled(bool enabled)
{
    gColourEnabled = enabled;
}

static fmt::text_style to_text_style(Colour c)
{
    fmt::text_style style;
    switch (c & Colour::COLOUR_MASK)
    {
        case Colour::BLACK:     style = fg(fmt::color::black); break;
        case Colour::GREY:      style = fg(fmt::color::gray); break;
        case Colour::YELLOW:    style = fg(fmt::color::yellow); break;
        case Colour::BLUE:      style = fg(fmt::color::blue); break;
        case Colour::GREEN:     style = fg(fmt::color::green); break;
        case Colour::RED:       style = fg(fmt::color::crimson); break;
        case Colour::MAGENTA:   style = fg(fmt::color::magenta); break;
        case Colour::PURPLE:    style = fg(fmt::color::medium_purple); break;
        case Colour::WHITE:     style = fg(fmt::color::white); break;
        default:                style = fg(fmt::color::white); break;
    }
    if ((c & Colour::BOLD) != (Colour)0)
    {
        style |= fmt::emphasis::bold;
    }
    return style;
}

string colour(Colour c, std::string_view str)
{
    if (!gColourEnabled)
    {
        return string(str);
    }
    return fmt::format(to_text_style(c), "{}", str);
}

Window::Window(size_t width, size_t height, Colour defaultColour)
    : _current(defaultColour), _default(defaultColour), _width(width), 
    _height(height)
{
    _buf = std::unique_ptr<Cell[]>(new Cell[width * height]);
}

Colour Window::set_colour(Colour newColour) 
{
    Colour old = _current;
    _current = newColour;
    return old;
}

void Window::draw_char(size_t x, size_t y, char ch, size_t count) 
{
    assert(y < _height);
    assert(x + count <= _width); // hopefully we made diagram wide enough
    for (size_t i = 0; i < count; ++i) 
    {
        at(x + i, y) = Cell(_current, ch);
    }
}

void Window::draw_string(size_t x, size_t y, string_view str) 
{
    assert(y < _height);
    assert(x + str.size() <= _width); // hopefully we made diagram wide enough
    for (size_t i = 0; i < str.size(); ++i) 
    {
        at(x + i, y) = Cell(_current, str[i]);
    }
}

bool Window::print(std::ostream& dest, bool useColour) const 
{
    size_t width = _width;
    size_t height; // ignore
    bool truncated = false;

    if (get_terminal_size(width, height))
    {
        truncated = (width < _width);
        width = std::min(width, _width);
    }

    for (size_t row = 0; row < _height; ++row) 
    {
        for (size_t col = 0; col < width; ++col) 
        {
            Cell cell = get_cell(col, row);
            if (useColour)
            {
                dest << colour(cell.colour, string_view(&cell.ch, 1));
            }
            else
            {
                dest << cell.ch;
            }
        }
        dest << '\n';
    }
    dest.flush();

    return !truncated;
}

bool init_curses_colour() 
{
    return start_color() != ERR
        && assume_default_colors(-1, -1) != ERR
        && init_pair(1, COLOR_BLACK, -1) != ERR
        && init_pair(2, COLOR_YELLOW, -1) != ERR
        && init_pair(3, COLOR_BLUE, -1) != ERR
        && init_pair(4, COLOR_GREEN, -1) != ERR
        && init_pair(5, COLOR_RED, -1) != ERR
        && init_pair(6, COLOR_MAGENTA, -1) != ERR
        && init_pair(7, COLOR_WHITE, -1) != ERR
        && init_pair(8, COLOR_RED, COLOR_WHITE) != ERR;
}

int get_message_colour() 
{
    return COLOR_PAIR(8);
}

int get_colour(Colour c) 
{
    int attr = 0;
    if ((c & Colour::BOLD) != (Colour)0)
    {
        attr |= A_BOLD;
    }
    switch (c & Colour::COLOUR_MASK) 
    {
        case Colour::BLACK:     return attr | COLOR_PAIR(1);
        case Colour::GREY:      return A_BOLD | COLOR_PAIR(1);
        case Colour::YELLOW:    return attr | COLOR_PAIR(2);
        case Colour::BLUE:      return attr | COLOR_PAIR(3);
        case Colour::GREEN:     return attr | COLOR_PAIR(4);
        case Colour::RED:       return attr | COLOR_PAIR(5);
        case Colour::MAGENTA:   return attr | COLOR_PAIR(6);
        case Colour::WHITE:     return attr | COLOR_PAIR(7);
        default:
            assert(!"Colour is not supported for ScrollView"); // i'm lazy
    }
}

void ScrollView::draw_window(bool resized)
{
    int width, height;
    getmaxyx(stdscr, height, width); // macro
    if (width < 0 || height < 0)
    {
        throw runtime_error("Failed to query window size of stdscr");
    }

    if (resized) 
    {
        clear();
        attron(get_message_colour());
        mvaddstr(height - 1, 0, _helpMessage.c_str());
        attroff(get_message_colour());
        refresh();
    }

    for (size_t i = 0; i < ARRAY_SIZE(_lines); ++i)
    {
        if (_lines[i].length() > (size_t)width) 
        {
            mvaddnstr(i, 0, _lines[i].c_str(), width - 3);
            addstr("...");
        } 
        else 
        {
            mvaddstr(i, 0, _lines[i].c_str());
            clrtoeol();
        }
    }
    
    // Calculate the pad offset so that it is centered in the middle. The pad
    // offset is the coordinates on the pad which will correspond to the top
    // left corner of the section of the screen that we draw it on.
    size_t padOffsetX = 0, padOffsetY = 0;
    if (_cursorX > ((size_t)width / 2)) 
    {
        padOffsetX = _cursorX - (width / 2);
    }
    if (_cursorY > ((size_t)height / 2)) 
    {
        padOffsetY = _cursorY - (height / 2);
    }

    // Calculate the screen area available for the pad, as well as the coords
    // where we'll draw the pad. We need to account for the rows of info text.
    size_t padPosX = 0;
    size_t padPosY = 2; // 2 lines at top
    size_t availableWidth = width;
    size_t availableHeight = height - 3; // 2 lines at top, 1 at bottom

    // Calculate how many rows/columns are visible off the edge of the pad.
    // (which we'll call the "overhang").We'll need to clear these ourselves 
    // since drawing the pad won't do it (since the pad doesn't extend to 
    // those rows/columns).
    //
    // We'll zero out the overhang in two segments (if they exist) by looping
    // through each line and clearing to the end of the line. The two sections
    // (labelled BOTTOM and RIGHT) are illustrated in the below diagram:
    //
    //  +---------------------------------------------------+------------+
    //  |                                                   |            |
    //  |                                                   |            |
    //  |                                                   |            |
    //  |                                                   |   RIGHT    |
    //  |                                                   |  OVERHANG  |
    //  |                   PAD AREA                        |            |
    //  |                                                   |            |
    //  |                                                   |            |
    //  |                                                   |            |
    //  |                                                   |            |
    //  +---------------------------------------------------+------------+
    //  |                                                                |
    //  |                                                                |
    //  |                      BOTTOM OVERHANG                           |
    //  |                                                                |
    //  +----------------------------------------------------------------+
    //
    // Just remember - these overhang sections are only visible if the diagram
    // is scrolled far enough to the right or bottom edges.

    // Calculate the overhang.
    size_t overhangX = std::max(0UL, padOffsetX + availableWidth - _padWidth);
    size_t overhangY = std::max(0UL, padOffsetY + availableHeight - _padHeight);

    // Zero out the BOTTOM section.
    size_t yStart = padPosY + availableHeight - overhangY;
    size_t yMax = padPosY + availableHeight;
    for (size_t y = yStart; y < yMax; ++y) 
    {
        move(y, 0);
        clrtoeol();
    }

    // Zero out the RIGHT section.
    if (overhangX > 0) 
    {
        yStart = padPosY;
        yMax = padPosY + availableHeight - overhangY;
        for (size_t y = yStart; y < yMax; ++y) 
        {
            move(y, padPosX + availableWidth - overhangX);
            clrtoeol();
        }
    }

    // Redraw the pad on the screen with the updated offset etc.
    prefresh(_pad, padOffsetY, padOffsetX, padPosY, padPosX, 
        height - 2, width - 1); // subtract 2 from height due to row at bottom
    // Set cursor position (relative to terminal screen).
    move(_cursorY + padPosY - padOffsetY, _cursorX + padPosX - padOffsetX);
}

void ScrollView::run() 
{
    assert(_pad && _padWidth > 0 && _padHeight > 0);
    draw_window(true);
    while (_running) 
    {
        int c = getch();
        if (c != KEY_RESIZE) 
        {
            _keyHandler(*this, c);
        }
        draw_window(c == KEY_RESIZE);
    }
}

void ScrollView::build_image(const Window& image) 
{
    if (_pad) 
    {
        delwin(_pad);
        _pad = nullptr;
    }

    _padWidth = image.width();
    _padHeight = image.height();
    _pad = newpad(_padHeight, _padWidth);
    if (!_pad) 
    {
        throw runtime_error("Failed to create curses pad.");
    }
    keypad(_pad, TRUE);

    int attr = get_colour(Colour::RESET);
    wattron(_pad, attr);

    for (size_t y = 0; y < image.height(); ++y) 
    {
        mvwin(_pad, y, 0);
        for (size_t x = 0; x < image.width(); ++x) 
        {
            Window::Cell cell = image.get_cell(x, y);
            int newAttr = get_colour(cell.colour);
            if (newAttr != attr) 
            {
                wattroff(_pad, attr);
                wattron(_pad, newAttr);
                attr = newAttr;
            }
            mvwaddch(_pad, y, x, cell.ch);
        }
    }
}

void ScrollView::update(const Window& image) 
{
    build_image(image);
    draw_window(true);
}

void ScrollView::cleanup() 
{
    if (_pad) 
    {
        delwin(_pad);
        _pad = nullptr;
    }
    keypad(stdscr, FALSE);
    nocbreak();
    echo();
    endwin();
}

ScrollView::ScrollView(const Window& image, 
                       string_view helpMessage, 
                       KeyCallback onKey) 
    : _pad(nullptr), _padWidth(0), _padHeight(0), _cursorX(0), _cursorY(0), 
    _running(true), _helpMessage(helpMessage), _keyHandler(onKey)
{
    if (!initscr() || cbreak() == ERR || noecho() == ERR 
        || keypad(stdscr, TRUE) == ERR)
    {
        throw runtime_error("Failed to initialise curses window.");
    }
    if (!init_curses_colour()) 
    {
        cleanup();
        throw runtime_error("Failed to initialise curses colours.");
    }
    
    clear();

    if (can_change_color() == TRUE) 
    {
        init_pair(0, 0, 0);
    } 
    else 
    {
        cleanup();
        throw runtime_error("BALLS");
    }

    try 
    {
        update(image);
    } 
    catch (const std::exception& e) 
    {
        cleanup();
        throw e;
    }
}

void ScrollView::set_line(string_view line, size_t y) 
{
    assert(y < ARRAY_SIZE(_lines));
    _lines[y].assign(line);
}

void ScrollView::set_cursor(size_t x, size_t y) 
{
    assert(x < _padWidth && y < _padHeight);
    _cursorX = x;
    _cursorY = y;
}

void ScrollView::beep() 
{
    if (isatty(STDOUT_FILENO))
    {
        char ch = '\a';
        write(STDOUT_FILENO, &ch, sizeof(ch)); // yeet
    }
}

void restore_terminal() 
{
    keypad(stdscr, FALSE);
    nocbreak();
    echo();
    endwin();
    // TODO dodgy?
}

bool get_terminal_size(size_t& width, size_t& height)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1)
    {
        return false;
    }
    width = size.ws_col;
    height = size.ws_row;
    return true;
}

string wrap_text_to_screen(string_view text, 
                           bool justify,
                           size_t indent, 
                           size_t maxWidth) 
{
    if (maxWidth == 0)
    {
        maxWidth = std::numeric_limits<size_t>::max();
    }
    size_t width, height;
    if (!get_terminal_size(width, height))
    {
        width = maxWidth;
    }
    width = std::min(maxWidth, width);
    return wrap_text(text, std::max(width, indent) - indent, indent, justify);
}
