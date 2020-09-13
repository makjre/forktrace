/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  text-wrap
 *
 *      I hate ugly line wrapping, and hardcoding all the line wrapping for the
 *      various help messages is tiring, so I wrote this 'module' that does it
 *      automatically. There have been whole papers written about how to break
 *      a paragraph into separate lines and there are existing algorithms out
 *      there (I don't know if there's any C/C++ libraries for it).
 *
 *      Just gonna go with a greedy algorithm and I'll mask the deficiencies
 *      by justifying the text and hoping the screen width is wide enough for
 *      it all to average out and look good enough.
 */
#include <cassert>

// TODO proof-read this code. lol.

#include "text-wrap.hpp"
#include "util.hpp"

using std::string;
using std::string_view;
using std::vector;

/* Ad hoc struct used by the below functions. */
struct Line
{
    /* These are indices of the words that form the start and end of the line.
     * These indices are into the list of words for the paragraph that contains
     * this line (see the Paragraph struct below). */
    size_t firstWord;
    size_t lastWord;
};

/* Ad hoc struct used by the below functions. */
struct Paragraph
{
    vector<string_view> words;
    vector<Line> lines;
};

/* Implements the greedy algorithm. This consists of fitting as many words we
 * can onto the first line, then doing the same for the next line, and so on.
 * The returned Line structs describe the indices of the words that each line
 * consists of. `width` is the maximum length of each line. If a word is too
 * long for a single line, then we'll put that word on its own line. */
vector<Line> assign_lines_greedily(const vector<string_view>& words, 
                                   size_t width)
{
    if (words.empty())
    {
        return {};
    }
    vector<Line> lines;
    size_t len = words[0].size(); // current line length
    Line line; // the current line
    line.firstWord = 0; // the line with words[0] only
    line.lastWord = 0;

    for (size_t i = 1; i < words.size(); ++i)
    {
        size_t newLen = len + 1 + words[i].size();
        if (newLen > width)
        {
            // The current line is at full capacity, start a new line.
            lines.push_back(line);
            len = words[i].size();
            line.firstWord = i;
        }
        else
        {
            len = newLen;
        }
        line.lastWord = i;
    }
    lines.push_back(line); // add the last line
    return lines;
}

/* Figures out where we should place line breaks in the provided paragraph of
 * words. The returned Line structs describe the indices of the words that each
 * line consists of. `width` is the maximum line length */
vector<Line> assign_lines(const vector<string_view>& words, size_t width)
{
    vector<Line> lines = assign_lines_greedily(words, width);
    // TODO maybe can add some more advanced processing here if I care enough
    return lines;
}

/* Appends a justified version of the specified line of the paragraph to the
 * output string. The line will be justified to the specified width. */
void render_line_of_paragraph(string& output, 
                              const Line& line, 
                              const Paragraph& paragraph,
                              size_t width)
{
    // Find the total length of the words in the line
    size_t len = 0;
    for (size_t i = line.firstWord; i <= line.lastWord; ++i)
    {
        len += paragraph.words.at(i).size();
    }

    assert(line.firstWord <= line.lastWord);
    size_t numWords = line.lastWord - line.firstWord + 1;

    // The line assignment algorithm should have assigned words so that there
    // was enough space on each line to contain the words plus the intervening
    // spaces (at least). The only way that wouldn't have happened would be if
    // there was a word longer than the screen width, in which case the line
    // should only have a single word on it.
    if (len + (numWords - 1) > width)
    {
        assert(line.firstWord == line.lastWord);
        assert(paragraph.words.at(line.firstWord).size() > width);
        output += paragraph.words.at(line.firstWord);
        return;
    }
    // Don't bother justifying the text if there's only one word. By filtering
    // this out now, we avoid a potential divide-by-zero later on.
    if (numWords == 1)
    {
        output += paragraph.words.at(line.firstWord);
        return;
    }

    // Only justify if this isn't the last line of the paragraph
    bool justify = (line.lastWord + 1 < paragraph.words.size());
    // Figure out how many spaces we can fit per word.
    size_t spacesPerWord = justify ? ((width - len) / (numWords - 1)) : 1;
    // There may be some remainder left over.
    size_t remainingSpaces = justify ? ((width - len) % (numWords - 1)) : 0;

    for (size_t i = line.firstWord; i < line.lastWord; ++i)
    {
        output += paragraph.words.at(i);
        for (size_t j = 0; j < spacesPerWord; ++j)
        {
            output += ' ';
        }
        if (remainingSpaces > 0)
        {
            output += ' ';
            remainingSpaces--;
        }
    }
    output += paragraph.words.at(line.lastWord); // add the last word
}

string wrap_text(string_view text, size_t width, size_t indent, bool justify)
{
    // Split our text into paragraphs of text (each line becomes a paragraph).
    // We don't want to merge consecutive newline delimiters, hence 'false'.
    vector<string_view> textPerParagraph = split_views(text, '\n', false);
    vector<Paragraph> paragraphs;
    paragraphs.reserve(textPerParagraph.size());
    size_t totalLineCount = 0;
    size_t textWidth = std::max(width, indent) - indent;

    for (string_view paragraphText : textPerParagraph)
    {
        Paragraph paragraph;
        paragraph.words = split_views(paragraphText, ' ');
        paragraph.lines = assign_lines(paragraph.words, textWidth);
        paragraphs.push_back(std::move(paragraph));
        totalLineCount += paragraph.lines.size();
    }

    string output;
    output.reserve(totalLineCount * (indent + textWidth + 1));
    for (Paragraph& paragraph : paragraphs)
    {
        if (paragraph.lines.empty()) // occurs for blank lines
        {
            output += '\n';
            continue;
        }
        for (Line& line : paragraph.lines)
        {
            for (size_t i = 0; i < indent; ++i)
            {
                output += ' ';
            }
            if (justify)
            {
                render_line_of_paragraph(output, line, paragraph, textWidth);
                output += '\n';
            }
            else
            {
                for (size_t i = line.firstWord; i <= line.lastWord; ++i)
                {
                    output += paragraph.words[i];
                    output += (i == line.lastWord ? '\n' : ' ');
                }
            }
        }
    }
    return output;
}
