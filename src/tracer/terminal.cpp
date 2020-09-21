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
