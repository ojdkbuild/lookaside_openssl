/* ====================================================================
 * Copyright (c) 2003 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.openssl.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@openssl.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.openssl.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#define _GNU_SOURCE

#include <openssl/rand.h>
#include <openssl/fips_rand.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/hmac.h>
#include <openssl/rsa.h>
#include <string.h>
#include <limits.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "fips_locl.h"

#ifdef OPENSSL_FIPS

# include <openssl/fips.h>

# ifndef PATH_MAX
#  define PATH_MAX 1024
# endif

static int fips_selftest_fail = 0;
static int fips_mode = 0;
static int fips_started = 0;

static int fips_is_owning_thread(void);
static int fips_set_owning_thread(void);
static int fips_clear_owning_thread(void);

# define fips_w_lock()   CRYPTO_w_lock(CRYPTO_LOCK_FIPS)
# define fips_w_unlock() CRYPTO_w_unlock(CRYPTO_LOCK_FIPS)
# define fips_r_lock()   CRYPTO_r_lock(CRYPTO_LOCK_FIPS)
# define fips_r_unlock() CRYPTO_r_unlock(CRYPTO_LOCK_FIPS)

static void fips_set_mode(int onoff)
{
    int owning_thread = fips_is_owning_thread();

    if (fips_started) {
        if (!owning_thread)
            fips_w_lock();
        fips_mode = onoff;
        if (!owning_thread)
            fips_w_unlock();
    }
}

int FIPS_module_mode(void)
{
    int ret = 0;
    int owning_thread = fips_is_owning_thread();

    if (fips_started) {
        if (!owning_thread)
            fips_r_lock();
        ret = fips_mode;
        if (!owning_thread)
            fips_r_unlock();
    }
    return ret;
}

/* just a compat symbol - return NULL */
const void *FIPS_rand_check(void)
{
    return NULL;
}

int FIPS_selftest_failed(void)
{
    int ret = 0;
    if (fips_started) {
        int owning_thread = fips_is_owning_thread();

        if (!owning_thread)
            fips_r_lock();
        ret = fips_selftest_fail;
        if (!owning_thread)
            fips_r_unlock();
    }
    return ret;
}

/* Selftest failure fatal exit routine. This will be called
 * during *any* cryptographic operation. It has the minimum
 * overhead possible to avoid too big a performance hit.
 */

void FIPS_selftest_check(void)
{
    if (fips_selftest_fail) {
        OpenSSLDie(__FILE__, __LINE__, "FATAL FIPS SELFTEST FAILURE");
    }
}

void fips_set_selftest_fail(void)
{
    fips_selftest_fail = 1;
}

/* we implement what libfipscheck does ourselves */

static int
get_library_path(const char *libname, const char *symbolname, char *path,
                 size_t pathlen)
{
    Dl_info info;
    void *dl, *sym;
    int rv = -1;

    dl = dlopen(libname, RTLD_LAZY);
    if (dl == NULL) {
        return -1;
    }

    sym = dlsym(dl, symbolname);

    if (sym != NULL && dladdr(sym, &info)) {
        strncpy(path, info.dli_fname, pathlen - 1);
        path[pathlen - 1] = '\0';
        rv = 0;
    }

    dlclose(dl);

    return rv;
}

static const char conv[] = "0123456789abcdef";

static char *bin2hex(void *buf, size_t len)
{
    char *hex, *p;
    unsigned char *src = buf;

    hex = malloc(len * 2 + 1);
    if (hex == NULL)
        return NULL;

    p = hex;

    while (len > 0) {
        unsigned c;

        c = *src;
        src++;

        *p = conv[c >> 4];
        ++p;
        *p = conv[c & 0x0f];
        ++p;
        --len;
    }
    *p = '\0';
    return hex;
}

# define HMAC_PREFIX "."
# ifndef HMAC_SUFFIX
#  define HMAC_SUFFIX ".hmac"
# endif
# define READ_BUFFER_LENGTH 16384

static char *make_hmac_path(const char *origpath)
{
    char *path, *p;
    const char *fn;

    path =
        malloc(sizeof(HMAC_PREFIX) + sizeof(HMAC_SUFFIX) + strlen(origpath));
    if (path == NULL) {
        return NULL;
    }

    fn = strrchr(origpath, '/');
    if (fn == NULL) {
        fn = origpath;
    } else {
        ++fn;
    }

    strncpy(path, origpath, fn - origpath);
    p = path + (fn - origpath);
    p = stpcpy(p, HMAC_PREFIX);
    p = stpcpy(p, fn);
    p = stpcpy(p, HMAC_SUFFIX);

    return path;
}

static const char hmackey[] = "orboDeJITITejsirpADONivirpUkvarP";

static int compute_file_hmac(const char *path, void **buf, size_t *hmaclen)
{
    FILE *f = NULL;
    int rv = -1;
    unsigned char rbuf[READ_BUFFER_LENGTH];
    size_t len;
    unsigned int hlen;
    HMAC_CTX c;

    HMAC_CTX_init(&c);

    f = fopen(path, "r");

    if (f == NULL) {
        goto end;
    }

    HMAC_Init(&c, hmackey, sizeof(hmackey) - 1, EVP_sha256());

    while ((len = fread(rbuf, 1, sizeof(rbuf), f)) != 0) {
        HMAC_Update(&c, rbuf, len);
    }

    len = sizeof(rbuf);
    /* reuse rbuf for hmac */
    HMAC_Final(&c, rbuf, &hlen);

    *buf = malloc(hlen);
    if (*buf == NULL) {
        goto end;
    }

    *hmaclen = hlen;

    memcpy(*buf, rbuf, hlen);

    rv = 0;
 end:
    HMAC_CTX_cleanup(&c);

    if (f)
        fclose(f);

    return rv;
}

static int FIPSCHECK_verify(const char *path)
{
    int rv = 0;
    FILE *hf;
    char *hmacpath, *p;
    char *hmac = NULL;
    size_t n;

    hmacpath = make_hmac_path(path);
    if (hmacpath == NULL)
        return 0;

    hf = fopen(hmacpath, "r");
    if (hf == NULL) {
        free(hmacpath);
        return 0;
    }

    if (getline(&hmac, &n, hf) > 0) {
        void *buf;
        size_t hmaclen;
        char *hex;

        if ((p = strchr(hmac, '\n')) != NULL)
            *p = '\0';

        if (compute_file_hmac(path, &buf, &hmaclen) < 0) {
            rv = -4;
            goto end;
        }

        if ((hex = bin2hex(buf, hmaclen)) == NULL) {
            free(buf);
            rv = -5;
            goto end;
        }

        if (strcmp(hex, hmac) != 0) {
            rv = -1;
        }
        free(buf);
        free(hex);
    } else {
        rv = -1;
    }

 end:
    free(hmac);
    free(hmacpath);
    fclose(hf);

    if (rv < 0)
        return 0;

    /* check successful */
    return 1;
}

static int verify_checksums(void)
{
    int rv;
    char path[PATH_MAX + 1];
    char *p;

    /* we need to avoid dlopening libssl, assume both libcrypto and libssl
       are in the same directory */

    rv = get_library_path("libcrypto.so." SHLIB_VERSION_NUMBER,
                          "FIPS_mode_set", path, sizeof(path));
    if (rv < 0)
        return 0;

    rv = FIPSCHECK_verify(path);
    if (!rv)
        return 0;

    /* replace libcrypto with libssl */
    while ((p = strstr(path, "libcrypto.so")) != NULL) {
        p = stpcpy(p, "libssl");
        memmove(p, p + 3, strlen(p + 2));
    }

    rv = FIPSCHECK_verify(path);
    if (!rv)
        return 0;
    return 1;
}

# ifndef FIPS_MODULE_PATH
#  define FIPS_MODULE_PATH "/etc/system-fips"
# endif

int FIPS_module_installed(void)
{
    int rv;
    rv = access(FIPS_MODULE_PATH, F_OK);
    if (rv < 0 && errno != ENOENT)
        rv = 0;

    /* Installed == true */
    return !rv;
}

int FIPS_module_mode_set(int onoff, const char *auth)
{
    int ret = 0;

    fips_w_lock();
    fips_started = 1;
    fips_set_owning_thread();

    if (onoff) {

        fips_selftest_fail = 0;

        /* Don't go into FIPS mode twice, just so we can do automagic
           seeding */
        if (FIPS_module_mode()) {
            FIPSerr(FIPS_F_FIPS_MODULE_MODE_SET,
                    FIPS_R_FIPS_MODE_ALREADY_SET);
            fips_selftest_fail = 1;
            ret = 0;
            goto end;
        }
# ifdef OPENSSL_IA32_SSE2
        {
            extern unsigned int OPENSSL_ia32cap_P[2];
            if ((OPENSSL_ia32cap_P[0] & (1 << 25 | 1 << 26)) !=
                (1 << 25 | 1 << 26)) {
                FIPSerr(FIPS_F_FIPS_MODULE_MODE_SET,
                        FIPS_R_UNSUPPORTED_PLATFORM);
                fips_selftest_fail = 1;
                ret = 0;
                goto end;
            }
        }
# endif

        if (!FIPS_selftest()) {
            fips_selftest_fail = 1;
            ret = 0;
            goto end;
        }

        if (!verify_checksums()) {
            FIPSerr(FIPS_F_FIPS_MODULE_MODE_SET,
                    FIPS_R_FINGERPRINT_DOES_NOT_MATCH);
            fips_selftest_fail = 1;
            ret = 0;
            goto end;
        }

        fips_set_mode(onoff);
        ret = 1;
        goto end;
    }
    fips_set_mode(0);
    fips_selftest_fail = 0;
    ret = 1;
 end:
    fips_clear_owning_thread();
    fips_w_unlock();
    return ret;
}

static CRYPTO_THREADID fips_thread;
static int fips_thread_set = 0;

static int fips_is_owning_thread(void)
{
    int ret = 0;

    if (fips_started) {
        CRYPTO_r_lock(CRYPTO_LOCK_FIPS2);
        if (fips_thread_set) {
            CRYPTO_THREADID cur;
            CRYPTO_THREADID_current(&cur);
            if (!CRYPTO_THREADID_cmp(&cur, &fips_thread))
                ret = 1;
        }
        CRYPTO_r_unlock(CRYPTO_LOCK_FIPS2);
    }
    return ret;
}

int fips_set_owning_thread(void)
{
    int ret = 0;

    if (fips_started) {
        CRYPTO_w_lock(CRYPTO_LOCK_FIPS2);
        if (!fips_thread_set) {
            CRYPTO_THREADID_current(&fips_thread);
            ret = 1;
            fips_thread_set = 1;
        }
        CRYPTO_w_unlock(CRYPTO_LOCK_FIPS2);
    }
    return ret;
}

int fips_clear_owning_thread(void)
{
    int ret = 0;

    if (fips_started) {
        CRYPTO_w_lock(CRYPTO_LOCK_FIPS2);
        if (fips_thread_set) {
            CRYPTO_THREADID cur;
            CRYPTO_THREADID_current(&cur);
            if (!CRYPTO_THREADID_cmp(&cur, &fips_thread))
                fips_thread_set = 0;
        }
        CRYPTO_w_unlock(CRYPTO_LOCK_FIPS2);
    }
    return ret;
}

# if 0
/* The purpose of this is to ensure the error code exists and the function
 * name is to keep the error checking script quiet
 */
void hash_final(void)
{
    FIPSerr(FIPS_F_HASH_FINAL, FIPS_R_NON_FIPS_METHOD);
}
# endif

#endif
