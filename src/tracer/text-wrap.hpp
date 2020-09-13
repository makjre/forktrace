/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  text-wrap
 *
 *      Functions to break paragraphs nicely into separate lines.
 */
#ifndef FORKTRACE_TEXT_WRAP_HPP
#define FORKTRACE_TEXT_WRAP_HPP

#include <string>

/* Wraps the provided text into the specified screen width. The resulting text
 * will have a newline at the end if there wasn't already one. Adds `indent`
 * occurrences of ' ' at the start of each resulting line. If justify=true, the
 * text will be 'justified' (meaning lines are all all made the same length).*/
std::string wrap_text(std::string_view text, 
                      size_t screenWidth, 
                      size_t indent = 0,
                      bool justify = false);

#endif /* FORKTRACE_TEXT_WRAP_HPP */
