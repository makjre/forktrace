/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  terminal
 *
 *      TODO
 */
#ifndef FORKTRACE_TERMINAL_HPP
#define FORKTRACE_TERMINAL_HPP

#include <string>

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
