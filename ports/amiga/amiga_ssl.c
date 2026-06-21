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

// AmiSSL v5 lifecycle.
//
// Mirrors modsocket.c's amiga_socket_open()/_close() shape. The
// global library bases live here; the AmiSSL SDK's stub trampolines
// (linked via -lamisslstubs) reach them through their normal extern
// references. No SSL-level functionality lives here yet — that
// arrives in Step 3 (modssl.c SSLContext).

#include "py/mpconfig.h"

#if MICROPY_PY_AMIGA_SSL

#include <stdbool.h>
#include <errno.h>
#include <proto/exec.h>
#include <proto/amisslmaster.h>
#include <libraries/amisslmaster.h>
#include <amissl/tags.h>

#include "amiga_ssl.h"

// SocketBase is defined in modsocket.c; AmiSSL needs the pointer
// supplied at open time so its internal socket calls route through
// the same bsdsocket.library we use.
extern struct Library *SocketBase;

struct Library *AmiSSLMasterBase;
struct Library *AmiSSLBase;
struct Library *AmiSSLExtBase;

int amiga_ssl_version_override;

bool amiga_ssl_open(void) {
    if (SocketBase == NULL) {
        // SSL on AmigaOS 3 requires a working bsdsocket.library; no
        // point trying without one.
        return false;
    }

    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
        AMISSLMASTER_MIN_VERSION);
    if (AmiSSLMasterBase == NULL) {
        return false;
    }

    // OpenAmiSSLTags handles the v5 master indirection: it picks the
    // matching amissl.library version (via AMISSL_CURRENT_VERSION
    // baked into the SDK we built against), opens it, and fills the
    // GetAmiSSLBase / GetAmiSSLExtBase out-params we pass.
    LONG api_version = amiga_ssl_version_override > 0
        ? (LONG)amiga_ssl_version_override
        : AMISSL_CURRENT_VERSION;
    LONG rc = OpenAmiSSLTags(
        api_version,
        AmiSSL_UsesOpenSSLStructs, FALSE,
        AmiSSL_GetAmiSSLBase,      (ULONG)&AmiSSLBase,
        AmiSSL_GetAmiSSLExtBase,   (ULONG)&AmiSSLExtBase,
        AmiSSL_SocketBase,         (ULONG)SocketBase,
        AmiSSL_ErrNoPtr,           (ULONG)&errno,
        TAG_DONE);
    if (rc != 0 || AmiSSLBase == NULL) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
        return false;
    }
    return true;
}

void amiga_ssl_close(void) {
    if (AmiSSLBase != NULL) {
        CloseAmiSSL();
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
    }
    if (AmiSSLMasterBase != NULL) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
}

#endif // MICROPY_PY_AMIGA_SSL
