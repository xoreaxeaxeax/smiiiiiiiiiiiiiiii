CC      = gcc
CFLAGS  = -O2 -Wall -mavx -march=native
TARGET  = smiiiiiiiiiiiiiiii

$(TARGET): smiiiiiiiiiiiiiiii.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: clean
