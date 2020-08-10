OUTPUTS = a.out tracer reaper

# Using C++20 support pretty much just for string::starts_with in command.cpp.
# Other than that, this uses quite a few features exclusive to C++14 / C++17.
CXX = g++
CXXFLAGS = -std=c++2a -g
LDFLAGS = -pthread -lncurses -lreadline
CC = gcc
CFLAGS = 

# Flags to generate dependency information
DEPFLAGS = -MMD -MP -MF"$(@:%.o=%.d)"

SRCS = main.cpp \
       tracer.cpp \
       system.cpp \
       tracee.cpp \
       process.cpp \
       event.cpp \
       util.cpp \
       diagram.cpp \
       terminal.cpp \
       command.cpp

BUILD_DIR = build
OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all
all: $(OUTPUTS)

.PHONY: clean
clean:
	rm -rf $(OUTPUTS)
	rm -rf $(BUILD_DIR)/*.o

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) $(DEPFLAGS) $< -o $@

tracer: $(OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@

reaper: reaper.c
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR):
	mkdir -p $@

a.out: example.c forktrace.h
	$(CC) $(CFLAGS) $^ -o $@

-include $(wildcard $(BUILD_DIR)/*.d)
