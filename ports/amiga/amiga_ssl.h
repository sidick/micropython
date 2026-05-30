// ports/amiga/amiga_ssl.h — AmiSSL v5 lifecycle and shared types.
//
// Phase 28 Step 2: provide the library-base globals and the
// open/close hooks called from main.c. Higher-level wrappers (Step 3
// SSLContext, Step 4 SSLSocket) consume the bases through the
// regular <proto/amissl.h> inlines.

#ifndef MICROPY_INCLUDED_AMIGA_SSL_H
#define MICROPY_INCLUDED_AMIGA_SSL_H

#include "py/mpconfig.h"

#if MICROPY_PY_AMIGA_SSL

#include <stdbool.h>
#include <exec/types.h>

// AmiSSL library bases. Referenced by the SDK's stub trampolines
// (linked from -lamisslstubs) via the standard "global handle"
// convention. We own the storage; amiga_ssl_open() fills them.
extern struct Library *AmiSSLMasterBase;
extern struct Library *AmiSSLBase;
extern struct Library *AmiSSLExtBase;

// Open amisslmaster.library, then OpenAmiSSLTags() with our SocketBase
// and errno pointer. Returns false if amisslmaster.library is missing
// (a user can run MicroPython without AmiSSL installed; `import ssl`
// will then raise ImportError). Must be called *after* the socket
// library is opened so SocketBase is valid.
bool amiga_ssl_open(void);

// Close in reverse order. Safe to call when amiga_ssl_open() failed
// or wasn't called.
void amiga_ssl_close(void);

// AmigaOS 3 callbacks must use BCPL-style argument passing (stack)
// and preserve a4/a5. bebbo gcc spells those `stkparm` and `saveds`.
// Wrap them in one macro so the Step 4 stream-protocol verify
// callback (if/when it lands) can be annotated portably.
#define AMISSL_CB __attribute__((stkparm)) __saveds

#endif // MICROPY_PY_AMIGA_SSL

#endif // MICROPY_INCLUDED_AMIGA_SSL_H
