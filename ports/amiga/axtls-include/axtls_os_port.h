// Port-local axtls_os_port.h, used only to compile lib/axtls/crypto/{md5,sha1}.c.
//
// The upstream shim at extmod/axtls-include/axtls_os_port.h is designed for
// the full MICROPY_PY_SSL + MICROPY_SSL_AXTLS path -- it pulls arpa/inet.h
// (for tls1.c's SOCKET_READ macro) and lib/crypto-algorithms/sha256.h (for
// the SHA256_CTX alias). On AmigaOS those two pull in exec/types.h via
// netinet/in.h, which collides with crypto-algorithms's BYTE / WORD
// typedefs.
//
// md5.c and sha1.c only need uint8_t / uint32_t in scope, so this shim
// stays minimal -- no socket macros, no SHA-256 aliases. Found ahead of
// the upstream copy by -I./axtls-include in the port Makefile.

#ifndef AXTLS_OS_PORT_H
#define AXTLS_OS_PORT_H

#include <stdint.h>

#endif // AXTLS_OS_PORT_H
