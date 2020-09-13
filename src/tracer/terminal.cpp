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

#include "log.hpp"
#include "text-wrap.hpp"
#include "terminal.hpp"

using std::string;
using std::string_view;

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
