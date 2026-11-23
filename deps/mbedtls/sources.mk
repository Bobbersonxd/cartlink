# Bundled mbedTLS sources (see CARTLINK-VERSION.txt), one list for every
# build that compiles them: Makefile.common (Linux, Windows/MinGW) and
# pkg/android/phoenix-common/jni/Android.mk.
#
# mbedTLS 2.28 can't be built as one unity file (static helpers with the
# same name in ssl_cli.c/ssl_srv.c and others), so griffin.c does not
# include these: each one is its own compile unit.

MBEDTLS_CRYPTO_SRCS := \
	aes.c aesni.c arc4.c asn1parse.c asn1write.c base64.c bignum.c \
	blowfish.c camellia.c ccm.c chacha20.c chachapoly.c cipher.c \
	cipher_wrap.c constant_time.c ctr_drbg.c des.c dhm.c ecdh.c ecdsa.c \
	ecp.c ecp_curves.c entropy.c entropy_poll.c gcm.c hmac_drbg.c md.c \
	md5.c oid.c padlock.c pem.c pk.c pk_wrap.c pkcs12.c pkcs5.c pkparse.c \
	pkwrite.c platform.c platform_util.c poly1305.c ripemd160.c rsa.c \
	rsa_internal.c sha1.c sha256.c sha512.c threading.c timing.c xtea.c

MBEDTLS_X509_SRCS := \
	certs.c x509.c x509_create.c x509_crl.c x509_crt.c x509_csr.c \
	x509write_crt.c x509write_csr.c

MBEDTLS_TLS_SRCS := \
	debug.c net_sockets.c ssl_cache.c ssl_ciphersuites.c ssl_cli.c \
	ssl_cookie.c ssl_msg.c ssl_srv.c ssl_ticket.c ssl_tls.c

MBEDTLS_SRCS := $(MBEDTLS_CRYPTO_SRCS) $(MBEDTLS_X509_SRCS) $(MBEDTLS_TLS_SRCS)
