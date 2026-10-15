CC     ?= gcc
CFLAGS := -O3 -march=native -std=gnu11 -Wall -pthread
bin/mem: src/mem.c src/chains.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@ -lm
clean:
	rm -rf bin
.PHONY: clean
