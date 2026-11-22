# Unit tests for network/url_content.c (pure logic, no network).
# Usage: make -C tests-other/url_content test

CC      ?= cc
LRC     := ../../libretro-common
CFLAGS  += -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O1 -g \
           -I$(LRC)/include -DHAVE_NETWORKING

SRCS := test_url_content.c \
        filestream_stub.c \
        ../../network/url_content.c \
        $(LRC)/hash/lrc_hash.c \
        $(LRC)/utils/md5.c \
        $(LRC)/encodings/encoding_base64.c \
        $(LRC)/compat/compat_strl.c

BIN := test_url_content

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) ../../network/url_content.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
