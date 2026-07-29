# Paralax Captcha - Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude
LDFLAGS =
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)
TARGET = build/paralax-captcha

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build/*.o $(TARGET)

run: all
	./$(TARGET)
