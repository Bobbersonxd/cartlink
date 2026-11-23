/* Copyright (C) 2026 Cartlink contributors.
 *
 * Cartlink is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version. See COPYING.
 */

/* Bundled mbedTLS 2.28 for unity (griffin) builds such as Xcode, part
 * 1 of 6. mbedTLS can't be one unity file (static helpers with the
 * same name in several sources), so its sources (deps/mbedtls/
 * sources.mk) are split into griffin/griffin_mbedtls_1.c..6.c, each a
 * set that compiles together. Every source is in exactly one part.
 * net_sockets.c has a part of its own: it sets _POSIX_C_SOURCE and
 * _XOPEN_SOURCE, which only work before the first system header.
 * Needs deps/mbedtls on the include path (pkg/apple/BaseConfig.xcconfig).
 * Builds that compile sources.mk as separate files (Makefile.common,
 * Android.mk) don't use these. */

#if defined(HAVE_SSL) && defined(HAVE_NETWORKING) && defined(HAVE_BUILTINMBEDTLS)
#include "../deps/mbedtls/aes.c"
#include "../deps/mbedtls/aesni.c"
#include "../deps/mbedtls/arc4.c"
#include "../deps/mbedtls/asn1parse.c"
#include "../deps/mbedtls/asn1write.c"
#include "../deps/mbedtls/base64.c"
#include "../deps/mbedtls/bignum.c"
#include "../deps/mbedtls/blowfish.c"
#include "../deps/mbedtls/ccm.c"
#include "../deps/mbedtls/chacha20.c"
#include "../deps/mbedtls/chachapoly.c"
#include "../deps/mbedtls/cipher.c"
#include "../deps/mbedtls/cipher_wrap.c"
#include "../deps/mbedtls/constant_time.c"
#include "../deps/mbedtls/ctr_drbg.c"
#include "../deps/mbedtls/des.c"
#include "../deps/mbedtls/dhm.c"
#include "../deps/mbedtls/ecdh.c"
#include "../deps/mbedtls/ecdsa.c"
#include "../deps/mbedtls/ecp.c"
#include "../deps/mbedtls/ecp_curves.c"
#include "../deps/mbedtls/entropy.c"
#include "../deps/mbedtls/entropy_poll.c"
#include "../deps/mbedtls/gcm.c"
#include "../deps/mbedtls/hmac_drbg.c"
#include "../deps/mbedtls/md.c"
#include "../deps/mbedtls/md5.c"
#include "../deps/mbedtls/oid.c"
#include "../deps/mbedtls/padlock.c"
#include "../deps/mbedtls/pem.c"
#include "../deps/mbedtls/pk.c"
#include "../deps/mbedtls/pk_wrap.c"
#include "../deps/mbedtls/pkcs12.c"
#include "../deps/mbedtls/pkcs5.c"
#include "../deps/mbedtls/pkparse.c"
#include "../deps/mbedtls/pkwrite.c"
#include "../deps/mbedtls/platform.c"
#include "../deps/mbedtls/platform_util.c"
#include "../deps/mbedtls/poly1305.c"
#include "../deps/mbedtls/rsa.c"
#include "../deps/mbedtls/rsa_internal.c"
#include "../deps/mbedtls/threading.c"
#include "../deps/mbedtls/timing.c"
#include "../deps/mbedtls/xtea.c"
#include "../deps/mbedtls/certs.c"
#include "../deps/mbedtls/x509.c"
#include "../deps/mbedtls/x509_create.c"
#include "../deps/mbedtls/x509_crl.c"
#include "../deps/mbedtls/x509_csr.c"
#include "../deps/mbedtls/x509write_crt.c"
#include "../deps/mbedtls/x509write_csr.c"
#include "../deps/mbedtls/debug.c"
#include "../deps/mbedtls/ssl_cache.c"
#include "../deps/mbedtls/ssl_cli.c"
#include "../deps/mbedtls/ssl_cookie.c"
#include "../deps/mbedtls/ssl_msg.c"
#include "../deps/mbedtls/ssl_ticket.c"
#include "../deps/mbedtls/ssl_tls.c"
#endif
