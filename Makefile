# Makefile for building loadedlatency (which uses perfmeas functions)
CC = gcc
CFLAGS = -std=gnu99 -O3 -Wall -Wextra
LDFLAGS = -lnuma -lpthread

TARGET = loadedlatency
SRCS = loadedlatency.c perfmeasure.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)


