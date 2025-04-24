CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11
TARGET = sshell
SRC = sshell.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET) 