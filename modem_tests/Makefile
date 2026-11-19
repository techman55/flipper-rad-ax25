# Native unit + loopback tests for fap/modem (no Flipper headers needed).
# Usage: make test

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror
MODEM   := ../fap/modem
CFLAGS  += -I$(MODEM)
BUILD   := build

MODEM_SRC := $(wildcard $(MODEM)/*.c)
TESTS     := $(patsubst %.c,%,$(wildcard test_*.c))
BINS      := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test clean

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: %.c $(MODEM_SRC) $(wildcard $(MODEM)/*.h) testlib.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(MODEM_SRC) -lm

test: $(BINS)
	@fail=0; for t in $(BINS); do echo "== $$t"; ./$$t || fail=1; done; \
	if [ $$fail -eq 0 ]; then echo "ALL MODEM TESTS PASSED"; else echo "MODEM TESTS FAILED"; exit 1; fi

clean:
	rm -rf $(BUILD)
