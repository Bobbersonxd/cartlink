# Unit tests for network/url_content.c (pure logic, no network) and
# network/url_logins.c (temp file under /tmp).
# Usage: make -C tests-other/url_content test

CC      ?= cc
LRC     := ../../libretro-common
CFLAGS  += -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -O1 -g \
           -I$(LRC)/include -DHAVE_NETWORKING

SRCS := test_url_content.c \
        ../../network/url_content.c \
        ../../network/url_logins.c \
        $(LRC)/file/config_file.c \
        $(LRC)/file/file_path.c \
        $(LRC)/file/file_path_io.c \
        $(LRC)/streams/file_stream.c \
        $(LRC)/vfs/vfs_implementation.c \
        $(LRC)/string/stdstring.c \
        $(LRC)/compat/fopen_utf8.c \
        $(LRC)/compat/compat_posix_string.c \
        $(LRC)/time/rtime.c \
        $(LRC)/encodings/encoding_utf.c \
        $(LRC)/hash/lrc_hash.c \
        $(LRC)/utils/md5.c \
        $(LRC)/encodings/encoding_base64.c \
        $(LRC)/compat/compat_strl.c

BIN := test_url_content

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) ../../network/url_content.h ../../network/url_logins.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
