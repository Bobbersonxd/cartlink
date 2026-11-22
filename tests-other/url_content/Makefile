# Unit tests for network/url_content.c (pure logic, no network) and
# network/url_logins.c (temp file under /tmp).
# Usage: make -C tests-other/url_content test
#        make -C tests-other/url_content fetch-test
#        (network/url_fetch.c against serve.py on a free 127.0.0.1 port)

CC      ?= cc
LRC     := ../../libretro-common
CFLAGS  += -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -O1 -g \
           -I$(LRC)/include -DHAVE_NETWORKING

SRCS := test_url_content.c \
        ../../network/url_content.c \
        $(LRC)/formats/json/rjson.c \
        $(LRC)/streams/interface_stream.c \
        $(LRC)/streams/memory_stream.c \
        $(LRC)/encodings/encoding_crc32.c \
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

FETCH_SRCS := test_url_fetch.c \
        ../../network/url_fetch.c \
        ../../network/url_archive_org.c \
        ../../network/url_content.c \
        $(LRC)/formats/json/rjson.c \
        $(LRC)/streams/interface_stream.c \
        $(LRC)/streams/memory_stream.c \
        $(LRC)/encodings/encoding_crc32.c \
        ../../network/url_logins.c \
        $(LRC)/net/net_http.c \
        $(LRC)/net/net_compat.c \
        $(LRC)/net/net_socket.c \
        $(LRC)/lists/string_list.c \
        $(LRC)/features/features_cpu.c \
        $(LRC)/file/retro_dirent.c \
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

FETCH_BIN := test_url_fetch

.PHONY: all test fetch-test clean

all: $(BIN) $(FETCH_BIN)

$(BIN): $(SRCS) ../../network/url_content.h ../../network/url_logins.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

$(FETCH_BIN): $(FETCH_SRCS) ../../network/url_fetch.h ../../network/url_archive_org.h ../../network/url_content.h ../../network/url_logins.h
	$(CC) $(CFLAGS) -o $@ $(FETCH_SRCS)

# Starts serve.py on a free port, waits until it answers, runs the
# tests, and always kills the server (trap on EXIT).
fetch-test: $(FETCH_BIN)
	@port=$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'); \
	python3 ./serve.py $$port >/dev/null 2>&1 & pid=$$!; \
	trap 'kill $$pid 2>/dev/null; wait $$pid 2>/dev/null' EXIT INT TERM; \
	i=0; until python3 -c 'import socket,sys; socket.create_connection(("127.0.0.1", int(sys.argv[1])), 1).close()' $$port 2>/dev/null; do \
	   i=$$((i+1)); if [ $$i -gt 100 ]; then echo "serve.py did not start"; exit 1; fi; \
	   python3 -c 'import time; time.sleep(0.05)'; \
	done; \
	./$(FETCH_BIN) $$port

clean:
	rm -f $(BIN) $(FETCH_BIN)
