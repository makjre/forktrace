OUTPUTS = example forktrace reaper

# Need C++17 for this. Amongst other features of C++17, we use string_views all
# over the place! (I love string views - they're my favourite part of C++17).
CXX = g++
CXXFLAGS = -std=c++17 -Wall -g -Iinclude 
LDFLAGS = -pthread -lncurses -lreadline -lfmt
CC = gcc
CFLAGS = -std=gnu99 

# Flags to generate dependency information
DEPFLAGS = -MMD -MP -MF"$(@:%.o=%.d)"

# Where we'll put intermediate files (like .o files and .d files)
BUILD_DIR = build

# Source files for forktrace (.cpp only, we use gcc to generate header deps)
TRACER_SRCS = main.cpp \
	system.cpp \
	util.cpp \
        log.cpp \
        inject.cpp \
        text-wrap.cpp \
        forktrace.cpp \
        terminal.cpp \
        command.cpp \
        parse.cpp \
        process.cpp \
        event.cpp \
	ptrace.cpp \
        tracer.cpp

TRACER_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/tracer/%.o,$(TRACER_SRCS))

.PHONY: all
all: $(OUTPUTS)

.PHONY: clean
clean:
	rm -rf $(OUTPUTS)
	rm -rf $(BUILD_DIR)

###############################################################################
# forktrace et al
###############################################################################

$(BUILD_DIR):
	mkdir -p $@
	mkdir -p $@/tracer

$(BUILD_DIR)/tracer/%.o: src/tracer/%.cpp | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) -pedantic $(DEPFLAGS) $< -o $@

# Have a special target for system.cpp since -pedantic is annoying
$(BUILD_DIR)/tracer/system.o: src/tracer/system.cpp | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) $(DEPFLAGS) $< -o $@

src/tracer/injection.inc: src/forktrace.h
	# Generates a file that defines a C character array whose elements are
	# the bytes of the src/forktrace.h file. We can include this elsewhere.
	xxd -i $^ | head -n -1 > $@

forktrace: src/tracer/injection.inc $(TRACER_OBJS)
	$(CXX) $(TRACER_OBJS) $(LDFLAGS) -L. -o $@

###############################################################################
# reaper
###############################################################################

reaper: src/reaper/*.*
	$(CC) $(CFLAGS) `ls src/reaper/*.c` -o $@

###############################################################################
# example
###############################################################################

example: src/example.c src/forktrace.h
	$(CC) $(CFLAGS) $^ -o $@

###############################################################################
# Header dependencies for tracer et al
###############################################################################

-include $(wildcard $(BUILD_DIR)/*/*.d)
