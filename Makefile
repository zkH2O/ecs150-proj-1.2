CC=gcc
CFLAGS=-Wall -Wextra -Werror -std=c99
LDFLAGS=

SRCS=sshell.c
EXEC=sshell

all: $(EXEC)

$(EXEC): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(EXEC) $(LDFLAGS)

clean:
	rm -f $(EXEC) *.o core.*

.PHONY: all clean 