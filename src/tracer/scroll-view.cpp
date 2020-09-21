/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  scroll-view
 *
 *      TODO
 */
#include <unistd.h>
#include <cassert>

#include "terminal.hpp"
#include "scroll-view.hpp"
#include "util.hpp"

using std::string;
using std::string_view;
using std::runtime_error;

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
