#!/usr/bin/env bash

# Run the specified program in valgrind with full leak checking
# We ask it to generate suppressions for all the errors that occured
# We get valgrind to pass its output to a log file
valgrind --gen-suppressions=all --log-file=valgrind.raw --leak-check=full --show-leak-kinds=all $*

# Filter out the lines of output that aren't for suppressions
grep -v "^==" valgrind.raw > valgrind.supp
