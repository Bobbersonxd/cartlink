# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Unit tests for network/url_content.c (pure logic, no network) and
# network/url_logins.c + network/url_servers.c (temp files under /tmp).
# Usage: make -C tests-other/url_content test
#        make -C tests-other/url_content fetch-test
#        (network/url_fetch.c against serve.py on free 127.0.0.1 ports,
#        over http and https; the https test certificates are made once
#        with openssl into certs/)
#        make -C tests-other/url_content tls-online-test
#        (needs internet: certificate checks against real sites)
#        make -C tests-other/url_content win-test [WIN_TMP=<dir>]
#        (url_logins on Windows, DPAPI: built with MinGW, run under Wine
#        in a throwaway prefix under WIN_TMP, default obj-win/; headless,
#        silent; skipped with a note if MinGW or Wine is missing)

CC      ?= cc
AR      ?= ar
LRC     := ../../libretro-common
CFLAGS  += -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -O1 -g \
           -I$(LRC)/include -DHAVE_NETWORKING

# network/url_safety.c reads .7z headers with the bundled LZMA SDK and
# inflates the first bytes of .zip members with the system zlib.
SZ_DIR  := ../../deps/7zip
SZ_SRCS := $(addprefix $(SZ_DIR)/,7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c \
           CpuArch.c Delta.c LzFind.c LzmaDec.c Lzma2Dec.c Bra.c Bra86.c \
           BraIA64.c Bcj2.c 7zFile.c 7zStream.c)
SZ_OBJS := $(patsubst $(SZ_DIR)/%.c,obj-7z/%.o,$(SZ_SRCS))
SZ_LIB  := obj-7z/lib7z.a
# url_safety.c alone is built with zlib + 7z on (the rest of the test
# sources must not see HAVE_ZLIB: it pulls in rzip streams).
SAFETY_OBJ  := obj-7z/url_safety.o
SAFETY_LIBS := $(SAFETY_OBJ) $(SZ_LIB) -lz

SRCS := test_url_content.c \
        ../../network/url_content.c \
        ../../network/url_catalog.c \
        ../../frontend/clipboard.c \
        $(LRC)/formats/json/rjson.c \
        $(LRC)/streams/interface_stream.c \
        $(LRC)/streams/memory_stream.c \
        $(LRC)/encodings/encoding_crc32.c \
        ../../network/url_logins.c \
        ../../network/url_servers.c \
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

# check_link: one link through url_content_split_fingerprint (used by
# tools/cartlink-server/test_server.sh to check cartlink-link's output).
CHECK_BIN  := check_link
CHECK_SRCS := check_link.c $(filter-out test_url_content.c,$(SRCS))

# The fetch test has https: RetroArch's bundled mbedTLS, built once
# into obj-mbed/.
MBED_DIR     := ../../deps/mbedtls
MBED_SRCS    := $(wildcard $(MBED_DIR)/*.c)
MBED_OBJS    := $(patsubst $(MBED_DIR)/%.c,obj-mbed/%.o,$(MBED_SRCS))
MBED_LIB     := obj-mbed/libmbed.a
FETCH_CFLAGS := $(CFLAGS) -DHAVE_SSL -DHAVE_BUILTINMBEDTLS -I$(MBED_DIR)

FETCH_SRCS := test_url_fetch.c \
        ../../network/url_fetch.c \
        ../../network/url_archive_org.c \
        ../../network/url_content.c \
        ../../network/url_catalog.c \
        $(LRC)/formats/json/rjson.c \
        $(LRC)/streams/interface_stream.c \
        $(LRC)/streams/memory_stream.c \
        $(LRC)/encodings/encoding_crc32.c \
        ../../network/url_logins.c \
        $(LRC)/net/net_http.c \
        $(LRC)/net/net_compat.c \
        $(LRC)/net/net_socket.c \
        $(LRC)/net/net_socket_ssl_mbed.c \
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

# Throwaway test CA and a "localhost" certificate signed by it.
CERTS := certs/ca.pem certs/srv.pem certs/srv.key

.PHONY: all test fetch-test tls-online-test win-test check-link clean

all: $(BIN) $(FETCH_BIN)

# --wrap: test_url_content.c sees every rename (counts them, acts like
# Windows, fails chosen ones).
obj-7z/%.o: $(SZ_DIR)/%.c
	@mkdir -p obj-7z
	@$(CC) -O1 -w -D_7ZIP_ST -I$(SZ_DIR) -c -o $@ $<

$(SZ_LIB): $(SZ_OBJS)
	@$(AR) rcs $@ $(SZ_OBJS)

$(SAFETY_OBJ): ../../network/url_safety.c ../../network/url_safety.h
	@mkdir -p obj-7z
	$(CC) $(CFLAGS) -I../../deps -DHAVE_ZLIB -DHAVE_7ZIP -D_7ZIP_ST -c -o $@ $<

$(BIN): $(SRCS) $(SAFETY_OBJ) $(SZ_LIB) ../../network/url_content.h ../../network/url_catalog.h ../../network/url_logins.h ../../network/url_servers.h ../../network/url_safety.h ../../frontend/clipboard.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(SAFETY_LIBS) -Wl,--wrap=filestream_rename

# make_archives.py builds the archive fixtures (dummy bytes only) in a
# fresh temp folder, runs the tests on it and removes it again.
# test_archive_agree: url_safety against RetroArch's own zip/7z reader.
AGREE_BIN  := test_archive_agree
AGREE_SRCS := test_archive_agree.c \
        $(LRC)/file/archive_file.c \
        $(LRC)/file/archive_file_zlib.c \
        $(LRC)/file/archive_file_7z.c \
        $(LRC)/streams/trans_stream.c \
        $(LRC)/streams/trans_stream_pipe.c \
        $(LRC)/streams/trans_stream_zlib.c \
        $(LRC)/streams/file_stream.c \
        $(LRC)/vfs/vfs_implementation.c \
        $(LRC)/file/file_path.c \
        $(LRC)/file/file_path_io.c \
        $(LRC)/string/stdstring.c \
        $(LRC)/lists/string_list.c \
        $(LRC)/compat/compat_strl.c \
        $(LRC)/compat/fopen_utf8.c \
        $(LRC)/compat/compat_posix_string.c \
        $(LRC)/encodings/encoding_utf.c \
        $(LRC)/encodings/encoding_crc32.c \
        $(LRC)/time/rtime.c

$(AGREE_BIN): $(AGREE_SRCS) $(SAFETY_OBJ) $(SZ_LIB) ../../network/url_safety.h
	$(CC) $(CFLAGS) -I../../deps -DHAVE_ZLIB -DHAVE_7ZIP -D_7ZIP_ST -o $@ $(AGREE_SRCS) $(SAFETY_LIBS) -Wl,--wrap=malloc

$(CHECK_BIN): $(CHECK_SRCS) ../../network/url_content.h
	$(CC) $(CFLAGS) -o $@ $(CHECK_SRCS)

check-link: $(CHECK_BIN)

test: $(BIN) $(AGREE_BIN)
	python3 -I ./make_archives.py --run ./$(BIN) ./$(AGREE_BIN)

obj-mbed/%.o: $(MBED_DIR)/%.c
	@mkdir -p obj-mbed
	@$(CC) -std=gnu99 -O1 -w -I$(MBED_DIR) -I$(LRC)/include -c -o $@ $<

$(MBED_LIB): $(MBED_OBJS)
	@$(AR) rcs $@ $(MBED_OBJS)

$(FETCH_BIN): $(FETCH_SRCS) $(MBED_LIB) $(SAFETY_OBJ) $(SZ_LIB) ../../network/url_safety.h ../../network/url_fetch.h ../../network/url_archive_org.h ../../network/url_catalog.h ../../network/url_content.h ../../network/url_logins.h $(LRC)/include/net/net_http.h $(LRC)/include/net/net_socket_ssl.h
	$(CC) $(FETCH_CFLAGS) -o $@ $(FETCH_SRCS) $(MBED_LIB) $(SAFETY_LIBS)

$(CERTS):
	@mkdir -p certs
	@cd certs && \
	openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 3650 \
	   -subj "/CN=Cartlink Test CA" -addext "basicConstraints=critical,CA:TRUE" \
	   -addext "keyUsage=critical,keyCertSign,cRLSign" >/dev/null 2>&1 && \
	openssl req -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr \
	   -subj "/CN=localhost" >/dev/null 2>&1 && \
	printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > ext.cnf && \
	openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
	   -out srv.pem -days 3650 -extfile ext.cnf >/dev/null 2>&1 \
	|| { echo "openssl could not make the test certificates"; exit 1; }

# Starts serve.py (http + https) on free ports, waits until both
# answer, runs the tests, and always kills the server (trap on EXIT).
fetch-test: $(FETCH_BIN) $(CERTS)
	@port=$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'); \
	sport=$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'); \
	python3 ./serve.py $$port $$sport certs/srv.pem certs/srv.key >/dev/null 2>&1 & pid=$$!; \
	trap 'kill $$pid 2>/dev/null; wait $$pid 2>/dev/null' EXIT INT TERM; \
	i=0; until python3 -c 'import socket,sys; [socket.create_connection(("127.0.0.1", int(p)), 1).close() for p in sys.argv[1:]]' $$port $$sport 2>/dev/null; do \
	   i=$$((i+1)); if [ $$i -gt 100 ]; then echo "serve.py did not start"; exit 1; fi; \
	   python3 -c 'import time; time.sleep(0.05)'; \
	done; \
	./$(FETCH_BIN) $$port $$sport certs/ca.pem

WIN_CC   ?= x86_64-w64-mingw32-gcc
WIN_TMP  ?= obj-win
WIN_BIN  := obj-win/test_logins_win.exe
WIN_SRCS := test_logins_win.c \
        ../../network/url_logins.c \
        ../../network/url_content.c \
        $(LRC)/file/config_file.c \
        $(LRC)/file/file_path.c \
        $(LRC)/file/file_path_io.c \
        $(LRC)/streams/file_stream.c \
        $(LRC)/vfs/vfs_implementation.c \
        $(LRC)/string/stdstring.c \
        $(LRC)/compat/fopen_utf8.c \
        $(LRC)/compat/compat_posix_string.c \
        $(LRC)/compat/compat_strl.c \
        $(LRC)/time/rtime.c \
        $(LRC)/encodings/encoding_utf.c \
        $(LRC)/encodings/encoding_base64.c \
        $(LRC)/encodings/encoding_crc32.c \
        $(LRC)/formats/json/rjson.c \
        $(LRC)/streams/interface_stream.c \
        $(LRC)/streams/memory_stream.c \
        $(LRC)/hash/lrc_hash.c \
        $(LRC)/utils/md5.c

$(WIN_BIN): $(WIN_SRCS) ../../network/url_logins.h ../../network/url_content.h
	@mkdir -p obj-win
	$(WIN_CC) -std=gnu99 -O1 -g -Wall -Wno-unused-parameter -I$(LRC)/include -DHAVE_NETWORKING \
	   -o $@ $(WIN_SRCS) -lcrypt32 -static

# test_ca_path_win.exe: the bundled cacert.pem next to the program,
# from a folder with a long non-ASCII name (net_socket_ssl_mbed.c and
# the bundled mbedTLS, built for Windows into obj-win/).
WINCA_BIN  := obj-win/test_ca_path_win.exe
WINCA_SRCS := test_ca_path_win.c \
        $(LRC)/net/net_socket_ssl_mbed.c \
        $(LRC)/net/net_socket.c \
        $(LRC)/net/net_compat.c \
        $(LRC)/features/features_cpu.c \
        $(LRC)/file/file_path.c \
        $(LRC)/file/file_path_io.c \
        $(LRC)/streams/file_stream.c \
        $(LRC)/vfs/vfs_implementation.c \
        $(LRC)/string/stdstring.c \
        $(LRC)/compat/fopen_utf8.c \
        $(LRC)/compat/compat_posix_string.c \
        $(LRC)/compat/compat_strl.c \
        $(LRC)/time/rtime.c \
        $(LRC)/encodings/encoding_utf.c
WIN_MBED_OBJS := $(patsubst $(MBED_DIR)/%.c,obj-win/mbed/%.o,$(MBED_SRCS))
WIN_MBED_LIB  := obj-win/mbed/libmbed.a
WIN_AR        ?= x86_64-w64-mingw32-ar
# 20 Cyrillic letters (40 UTF-8 bytes, 20 UTF-16 units), as in
# test_ca_path_win.c.
WINCA_DIR := $(shell printf '\320\220\320\273\320\265\320\272\321\201\320\260\320\275\320\264\321\200\320\237\320\265\321\202\321\200\320\276\320\262\320\270\321\207\320\241\320\270\320\264')

obj-win/mbed/%.o: $(MBED_DIR)/%.c
	@mkdir -p obj-win/mbed
	@$(WIN_CC) -std=gnu99 -O1 -w -I$(MBED_DIR) -I$(LRC)/include -c -o $@ $<

$(WIN_MBED_LIB): $(WIN_MBED_OBJS)
	@$(WIN_AR) rcs $@ $(WIN_MBED_OBJS)

# RARCH_INTERNAL: also checks file_path.c's fill_pathname_application_dir.
$(WINCA_BIN): $(WINCA_SRCS) $(WIN_MBED_LIB) $(LRC)/include/net/net_socket_ssl.h
	@mkdir -p obj-win
	$(WIN_CC) -std=gnu99 -O1 -g -Wall -Wno-unused-parameter -I$(LRC)/include -I$(MBED_DIR) \
	   -DHAVE_SSL -DHAVE_BUILTINMBEDTLS -DRARCH_INTERNAL \
	   -o $@ $(WINCA_SRCS) $(WIN_MBED_LIB) -lws2_32 -lcrypt32 -ladvapi32 -static

# Wine runs with its own prefix in WIN_TMP (never ~/.wine), no display,
# no debug output; the logins file goes to a fresh folder there.
win-test:
	@if ! command -v $(WIN_CC) >/dev/null 2>&1; then \
	   echo "SKIP: win-test ($(WIN_CC) not installed)"; exit 0; fi; \
	$(MAKE) --no-print-directory $(WIN_BIN) $(WINCA_BIN) $(CERTS) || exit 1; \
	if ! command -v wine >/dev/null 2>&1; then \
	   echo "SKIP: win-test run (wine not installed; $(WIN_BIN) built)"; exit 0; fi; \
	mkdir -p "$(WIN_TMP)/prefix" || exit 1; \
	data=$$(mktemp -d "$(abspath $(WIN_TMP))/data.XXXXXX") || exit 1; \
	env -u DISPLAY -u WAYLAND_DISPLAY WINEDEBUG=-all \
	   WINEPREFIX="$(abspath $(WIN_TMP))/prefix" \
	   WINEDLLOVERRIDES="mscoree,mshtml=" \
	   wine ./$(WIN_BIN) "Z:$$data" 2>/dev/null; rc=$$?; \
	cadir="$$data/$(WINCA_DIR)"; \
	mkdir -p "$$cadir" && cp $(WINCA_BIN) "$$cadir/cartlink.exe" && \
	{ echo "# CARTLINK-TEST-REAL-BUNDLE"; cat certs/ca.pem; } > "$$cadir/cacert.pem" && \
	{ echo "# PLANTED"; cat certs/ca.pem; } > "$$data/cacert.pem" || exit 1; \
	env -u DISPLAY -u WAYLAND_DISPLAY WINEDEBUG=-all \
	   WINEPREFIX="$(abspath $(WIN_TMP))/prefix" \
	   WINEDLLOVERRIDES="mscoree,mshtml=" \
	   wine "$$cadir/cartlink.exe" 2>/dev/null; rc2=$$?; \
	WINEPREFIX="$(abspath $(WIN_TMP))/prefix" wineserver -k >/dev/null 2>&1; \
	[ $$rc -eq 0 ] && [ $$rc2 -eq 0 ]

tls-online-test: $(FETCH_BIN)
	./$(FETCH_BIN) online ../../media/cacert.pem

clean:
	rm -f $(BIN) $(FETCH_BIN) $(AGREE_BIN) $(CHECK_BIN)
	rm -rf obj-mbed obj-7z obj-win certs
