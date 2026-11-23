/* Copyright (C) 2026 Cartlink contributors.
 *
 * Cartlink is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version. See COPYING.
 */

/* Bundled mbedTLS 2.28 for unity (griffin) builds such as Xcode, part
 * 2 of 6. mbedTLS can't be one unity file (static helpers with the
 * same name in several sources), so its sources (deps/mbedtls/
 * sources.mk) are split into griffin/griffin_mbedtls_1.c..6.c, each a
 * set that compiles together. Every source is in exactly one part.
 * net_sockets.c has a part of its own: it sets _POSIX_C_SOURCE and
 * _XOPEN_SOURCE, which only work before the first system header.
 * Needs deps/mbedtls on the include path (pkg/apple/BaseConfig.xcconfig).
 * Builds that compile sources.mk as separate files (Makefile.common,
 * Android.mk) don't use these. */

#if defined(HAVE_SSL) && defined(HAVE_NETWORKING) && defined(HAVE_BUILTINMBEDTLS)
#include "../deps/mbedtls/camellia.c"
#include "../deps/mbedtls/ripemd160.c"
#include "../deps/mbedtls/x509_crt.c"
#include "../deps/mbedtls/ssl_ciphersuites.c"
#include "../deps/mbedtls/ssl_srv.c"
#endif
