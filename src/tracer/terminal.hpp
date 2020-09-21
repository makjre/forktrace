/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  terminal
 *
 *      TODO
 */
#ifndef FORKTRACE_TERMINAL_HPP
#define FORKTRACE_TERMINAL_HPP

#include <string>
#include <iostream>
#include <memory>
#include <fmt/color.h>
#include <curses.h>

/* Why not just use libfmt's fmt::text_style type? It's huge (20 bytes). That 
 * wouldn't normally be a problem but if I'm drawing a huge diagram it would 
 * cause it to take up wayyyy more space. Also using my own type means less 
 * typing for me and can more easily work with curses (otherwise I'd have to 
 * write a function to convert fmt::text_style to curses colours). */
enum class Colour : uint8_t 
{
    /* Colours (whichever is 0 will be the default colour via DEFAULT but also
     * if you don't specify any colour - e.g., just Colour::BOLD). */
    WHITE   = 0,
    GREY    = 1,
    YELLOW  = 2,
    BLUE    = 3,
    GREEN   = 5,
    RED     = 6,
    MAGENTA = 7,
    PURPLE  = 8,
    BLACK   = 9,

    /* Emphasis */
    BOLD    = 0x80,

    /* Other */
    RESET = 0,
    DEFAULT = 0,
    COLOUR_MASK = 0x7F,
};

/* Define operator overloads so that we can use scoped enum like an integer. */
inline constexpr Colour operator|(Colour a, Colour b)
{
    return Colour(uint8_t(a) | uint8_t(b));
}

inline constexpr Colour operator&(Colour a, Colour b)
{
    return Colour(uint8_t(a) & uint8_t(b));
}

/* If false, then calls to colour() (below) will not add any colour. Despite
 * modifying global state, this function is thread-safe (atomic store). */
void set_colour_enabled(bool enabled);

/* Takes the string and applies escape codes to it so that terminals will show
 * it with the specified colour. Does nothing if set_colour_enabled(false). */
std::string colour(Colour colour, std::string_view str);

/* Just represents a grid of characters that we can draw to with colours of
 * a terminal. This can be printed out to the console or converted to a curses
 * pad for use with a TUI. I was going to just represent this directly with a
 * curses pad, but those can only exist while curses is set up, so it's easier
 * to just do it myself. */
class Window 
{
public:
    struct __attribute__((packed)) Cell 
    {
        Colour colour;
        char ch;
        Cell() : colour(Colour::DEFAULT), ch(' ') { }
        Cell(Colour c, char ch) : colour(c), ch(ch) { }
    };

private:
    Colour _current;
    Colour _default;
    size_t _width;
    size_t _height;
    std::unique_ptr<Cell[]> _buf; // stored as sequence of rows

    Cell& at(size_t x, size_t y) { return _buf[y * _width + x]; }

public:
    Window(size_t width, size_t height, Colour defaultColour = Colour::WHITE);
    Colour set_colour(Colour newColour);
    Colour reset_colour() { return set_colour(_default); }
    void draw_char(size_t x, size_t y, char ch, size_t count = 1);
    void draw_string(size_t x, size_t y, std::string_view str);
    Cell get_cell(size_t x, size_t y) const { return _buf[y * _width + x]; }
    size_t width() const { return _width; }
    size_t height() const { return _height; }

    /* Prints this window to dest. If colour==true, then this will use ANSI 
     * escape sequences to achieve the desired colours. Before printing, the 
     * function will query the current width of the window and truncate the 
     * output to fit within it. Returns false if had to be truncated. */
    bool print(std::ostream& dest, bool colour = true) const;
};

/* Queries the size of the terminal (specifically, this function will query
 * stdout, so if that file descriptor is not pointing to a terminal, then this
 * will fail). Returns false and sets errno on failure. */
bool get_terminal_size(size_t& width, size_t& height);

/* A helper method around wrap_text from text-wrap.hpp. Queries the terminal
 * size for us. If the size is larger than maxWidth, then the text is just
 * wrapped to maxWidth instead. If get_terminal_size fails, maxWidth is used.
 * Justifies the text if if justify is true. If maxWidth is unspecified (0),
 * then it is assumed that there is no maximum limit. */
std::string wrap_text_to_screen(std::string_view text, 
                                bool justify,
                                size_t indent, 
                                size_t maxWidth = 0);

#endif /* FORKTRACE_TERMINAL_HPP */
