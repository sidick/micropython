/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Simon Dick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

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

// Diagnostic: override the AMISSL APIVersion passed to OpenAmiSSLTags.
// Default (0) uses AMISSL_CURRENT_VERSION baked in from the SDK we
// built against. Set to e.g. AMISSL_V352 (=41 for SDK 5.27) to force
// the master library to negotiate a specific older amissl_vXYZ.library.
// Wired up via -X sslver=<N> in main.c, intended for bisecting the
// Cloudflare TLS 1.3 close.
extern int amiga_ssl_version_override;

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
