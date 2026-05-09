CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2
TARGET  = psx

SRCS    = main.c bios.c ram.c cop0.c interconnect.c cpu.c
OBJS    = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
