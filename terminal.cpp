#include <algorithm>
#include <cassert>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <iostream> // TODO
#include <sys/ioctl.h>

#include "terminal.h"
#include "util.h"

using namespace std;

static bool colourEnabled = true;

void setColourEnabled(bool enabled) {
    colourEnabled = enabled;
}

string_view getColourSequence(Colour c) {
    if (!colourEnabled) {
        return "";
    }
    switch (c) {
        // TODO ansi weird
        case Colour::BLACK:
            return "\033[30m";
        case Colour::GREY:
            return "\033[30;1m";
        case Colour::YELLOW:
            return "\033[33m";
        case Colour::BLUE:
            return "\033[34m";
        case Colour::BLUE_BOLD:
            return "\033[34;1m";
        case Colour::GREEN_BOLD:
            return "\033[32;1m";
        case Colour::RED:
            return "\033[31m";
        case Colour::RED_BOLD:
            return "\033[31;1m";
        case Colour::MAGENTA:
            return "\033[35m";
        case Colour::WHITE:
            return ""; // TODO explain
        case Colour::BOLD:
            return "\033[1m";
    }
    assert(!"Unreachable");
}

ostream& operator<<(ostream& os, Colour c) {
    if (colourEnabled) {
        os << "\033[0;0m" << getColourSequence(c);
    }
    return os;
}

string colourise(string_view s, Colour c) {
    string result(getColourSequence(c));
    result.append(s);
    result.append(colourEnabled ? "\033[0;0m" : "");
    return result;
}

Window::Window(size_t width, size_t height, Colour defaultColour)
    : _current(defaultColour), _default(defaultColour), _width(width), 
    _height(height)
{
    _buf = new Cell[width * height];
}

Window::~Window() {
    delete[] _buf;
}

Colour Window::setColour(Colour newColour) {
    Colour old = _current;
    _current = newColour;
    return old;
}

void Window::drawChar(size_t x, size_t y, char ch, size_t count) {
    assert(y < _height);
    assert(x + count <= _width);
    for (size_t i = 0; i < count; ++i) {
        at(x + i, y) = Cell(_current, ch);
    }
}

void Window::drawString(size_t x, size_t y, string_view str) {
    assert(y < _height);
    assert(x + str.size() <= _width);
    for (size_t i = 0; i < str.size(); ++i) {
        at(x + i, y) = Cell(_current, str[i]);
    }
}

bool Window::print(bool colour) const {
    struct winsize size;
    int ioErr = 0;
    size_t width = _width;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1) {
        ioErr = errno;
    } else {
        width = (_width > size.ws_col) ? size.ws_col : _width;
    }

    Colour c = _default;

    string_view reset = colourEnabled ? "\033[0;0m" : "";

    for (size_t row = 0; row < _height; ++row) {
        for (size_t col = 0; col < width; ++col) {
            Cell cell = getCell(col, row);

            if (c != cell.colour && colour) {
                c = cell.colour;
                cout << reset << getColourSequence(c);
            }
            
            cout << cell.ch;
        }
        if (colour) {
            cout << reset;
            c = Colour::WHITE;
        }
        cout << '\n';
    }
    cout.flush();

    if (ioErr != 0) {
        cout << "ioctl: " << strerror(ioErr) << endl;
        cout << "Couldn't query window size, "
            << "so I just drew the whole thing..." << endl;
        return true;
    }

    return _width <= size.ws_col;
}

bool initCursesColour() {
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

int getMessageColour() {
    return COLOR_PAIR(8);
}

int getColour(Colour c) {
    switch (c) {
        case Colour::BLACK:
            return COLOR_PAIR(1);
        case Colour::GREY:
            return COLOR_PAIR(1) | A_BOLD;
        case Colour::YELLOW:
            return COLOR_PAIR(2);
        case Colour::BLUE:
            return COLOR_PAIR(3);
        case Colour::BLUE_BOLD:
            return COLOR_PAIR(3) | A_BOLD;
        case Colour::GREEN_BOLD:
            return COLOR_PAIR(4) | A_BOLD;
        case Colour::RED:
            return COLOR_PAIR(5);
        case Colour::RED_BOLD:
            return COLOR_PAIR(5) | A_BOLD;
        case Colour::MAGENTA:
            return COLOR_PAIR(6);
        case Colour::WHITE:
            return COLOR_PAIR(7);
        case Colour::BOLD:
            return COLOR_PAIR(7) | A_BOLD;
    }
    assert(!"Unreachable");
}

void ScrollView::drawWindow(bool resized) {
    int width, height;
    getmaxyx(stdscr, height, width); // macro

    if (resized) {
        clear();
        attron(getMessageColour());
        mvaddstr(height - 1, 0, _helpMessage.c_str());
        attroff(getMessageColour());
        refresh();
    }

    for (size_t i = 0; i < ARRAY_SIZE(_lines); ++i) {
        if (_lines[i].length() > width) {
            mvaddnstr(i, 0, _lines[i].c_str(), width - 3);
            addstr("...");
        } else {
            mvaddstr(i, 0, _lines[i].c_str());
            clrtoeol();
        }
    }

    prefresh(_pad, _padY, _padX, 2, 0, height - 4, width - 1);
    move(_cursorY, _cursorX);
}

void ScrollView::run() {
    assert(_pad && _padWidth > 0 && _padHeight > 0);
    drawWindow(true);
    while (_running) {
        int c = getch();
        if (c != KEY_RESIZE) {
            _keyHandler(*this, c);
        }
        drawWindow(c == KEY_RESIZE);
    }
}

void ScrollView::buildImage(const Window& image) {
    if (_pad) {
        delwin(_pad);
        _pad = nullptr;
    }

    _padWidth = image.width();
    _padHeight = image.height();
    _pad = newpad(_padHeight, _padWidth);
    if (!_pad) {
        throw runtime_error("Failed to create curses pad.");
    }
    keypad(_pad, TRUE);

    int attr = getColour(Colour::RESET);
    wattron(_pad, attr);

    for (size_t y = 0; y < image.height(); ++y) {
        mvwin(_pad, y, 0);
        for (size_t x = 0; x < image.width(); ++x) {
            Window::Cell cell = image.getCell(x, y);
            int newAttr = getColour(cell.colour);

            if (newAttr != attr) {
                wattroff(_pad, attr);
                wattron(_pad, newAttr);
                attr = newAttr;
            }

            mvwaddch(_pad, y, x, cell.ch);
        }
    }
}

void ScrollView::update(const Window& image) {
    buildImage(image);
    drawWindow(true);
}

void ScrollView::cleanup() {
    if (_pad) {
        delwin(_pad);
        _pad = nullptr;
    }
    keypad(stdscr, FALSE);
    nocbreak();
    echo();
    endwin();
}

ScrollView::ScrollView(const Window& image, string helpMessage, 
        KeyCallback onKey, size_t margin) 
    : _pad(nullptr), _padWidth(0), _padHeight(0), _padX(0), _padY(0), 
    _cursorX(0), _cursorY(0), _scrollMargin(margin), _running(true), 
    _helpMessage(helpMessage), _keyHandler(onKey)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    //curs_set(0);

    if (!initCursesColour()) {
        cleanup();
        throw runtime_error("Failed to initialise curses colours.");
    }
    
    clear();

    if (can_change_color() == TRUE) {
        init_pair(0, 0, 0);
    } else {
        cleanup();
        throw runtime_error("BALLS");
    }

    try {
        update(image);
    } catch (const exception& e) {
        cleanup();
        throw e;
    }
}

void ScrollView::setLine(string_view line, size_t y) {
    assert(y < ARRAY_SIZE(_lines));
    _lines[y].assign(line);
}

void ScrollView::setCursor(size_t x, size_t y) {
    assert(x < _padWidth && y < _padHeight);
    
    // Calculate the pad offset so that it is centered in the middle.
    _padX = 0;
    _padY = 0; // TODO

    // Calculate the coordinates of the cursor
    _cursorX = x; // TODO
    _cursorY = y + 2;
}

void ScrollView::beep() {
    char ch = '\a';
    write(STDOUT_FILENO, &ch, sizeof(ch));
}

void restoreTerminal() {
    keypad(stdscr, FALSE);
    nocbreak();
    echo();
    endwin();
    // TODO dodgy?
}
