# Unit tests for network/url_content.c (pure logic, no network) and
# network/url_logins.c (temp file under /tmp).
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

SRCS := test_url_content.c \
        ../../network/url_content.c \
        ../../network/url_catalog.c \
        ../../frontend/clipboard.c \
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

$(BIN): $(SRCS) ../../network/url_content.h ../../network/url_catalog.h ../../network/url_logins.h ../../frontend/clipboard.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

obj-mbed/%.o: $(MBED_DIR)/%.c
	@mkdir -p obj-mbed
	@$(CC) -std=gnu99 -O1 -w -I$(MBED_DIR) -I$(LRC)/include -c -o $@ $<

$(MBED_LIB): $(MBED_OBJS)
	@$(AR) rcs $@ $(MBED_OBJS)

$(FETCH_BIN): $(FETCH_SRCS) $(MBED_LIB) ../../network/url_fetch.h ../../network/url_archive_org.h ../../network/url_content.h ../../network/url_logins.h $(LRC)/include/net/net_http.h $(LRC)/include/net/net_socket_ssl.h
	$(CC) $(FETCH_CFLAGS) -o $@ $(FETCH_SRCS) $(MBED_LIB)

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
	rm -f $(BIN) $(FETCH_BIN)
	rm -rf obj-mbed certs
