# Unit tests for network/url_content.c (pure logic, no network) and
# network/url_logins.c + network/url_servers.c (temp files under /tmp).
# Usage: make -C tests-other/url_content test
#        make -C tests-other/url_content fetch-test
#        (network/url_fetch.c against serve.py on free 127.0.0.1 ports,
#        over http and https; the https test certificates are made once
#        with openssl into certs/)
#        make -C tests-other/url_content tls-online-test
#        (needs internet: certificate checks against real sites)

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

.PHONY: all test fetch-test tls-online-test clean

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
	   -subj "/CN=Retro Link Test CA" -addext "basicConstraints=critical,CA:TRUE" \
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

tls-online-test: $(FETCH_BIN)
	./$(FETCH_BIN) online

clean:
	rm -f $(BIN) $(FETCH_BIN) $(AGREE_BIN)
	rm -rf obj-mbed obj-7z certs
