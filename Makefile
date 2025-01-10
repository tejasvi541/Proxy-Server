# Compiler and Flags
CC = gcc
CFLAGS = -Wall -Wextra -g -pthread  # Add -O2 for optimized builds
# If you're developing you can remove the -O2 flag, which will make debugging easier
# CFLAGS = -Wall -Wextra -g -pthread -O2
# Source files
SRCS = proxy_server_with_cache.c proxy_parse.c

# Header files
HEADERS = proxy_parse.h

# Object files
OBJS = $(SRCS:.c=.o)

# Executable name
TARGET = proxy

# Default target: build the executable
all: $(TARGET)

# Rule to compile object files from source files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to link object files into the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# Rule to clean up object files and executable
clean:
	rm -f $(OBJS) $(TARGET)

# Phony targets (not files)
.PHONY: all clean