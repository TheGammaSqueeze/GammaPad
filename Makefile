# Makefile for GammaPad
# Retains existing code plus new capture logic.
# Usage:
#   make
#   sudo ./gammapad [optional /dev/input/eventX]

# Set VERBOSE=1 if you want more debug logs for Force Feedback (FF).
VERBOSE ?= 0

CC = gcc
CFLAGS = -O2 -Wall -pthread -DGAMMAPAD_VERBOSE_LOGGING=$(VERBOSE)
TARGET = gammapad

SRCS = gammapad_main.c \
       gammapad_controller.c \
       gammapad_inputdefs.c \
       gammapad_ff.c \
       gammapad_commands.c \
       gammapad_capture.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c gammapad.h gammapad_inputdefs.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
