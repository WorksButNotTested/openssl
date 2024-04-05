/*
 * Copyright 2011-2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/* Copyright (c) 2016, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include "internal/cryptlib.h"

#include "arm_arch.h"

unsigned int OPENSSL_armcap_P = 0;
unsigned int OPENSSL_arm_midr = 0;
unsigned int OPENSSL_armv8_rsa_neonized = 0;

#if __ARM_MAX_ARCH__<7
void OPENSSL_cpuid_setup(void)
{
}

uint32_t OPENSSL_rdtsc(void)
{
    return 0;
}
#else
/*
 * Following subroutines could have been inlined, but it's not all
 * ARM compilers support inline assembler...
 */
void _armv7_neon_probe(void);
void _armv8_aes_probe(void);
void _armv8_sha1_probe(void);
void _armv8_sha256_probe(void);
void _armv8_pmull_probe(void);
# ifdef __aarch64__
void _armv8_sha512_probe(void);
unsigned int _armv8_cpuid_probe(void);
# endif
uint32_t _armv7_tick(void);

uint32_t OPENSSL_rdtsc(void)
{
    if (OPENSSL_armcap_P & ARMV7_TICK)
        return _armv7_tick();
    else
        return 0;
}

# if defined(__GNUC__) && __GNUC__>=2
void OPENSSL_cpuid_setup(void) __attribute__ ((constructor));
# endif

# if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 16)
#   include <sys/auxv.h>
#   define OSSL_IMPLEMENT_GETAUXVAL
#  endif
# elif defined(__ANDROID_API__)
/* see https://developer.android.google.cn/ndk/guides/cpu-features */
#  if __ANDROID_API__ >= 18
#   include <sys/auxv.h>
#   define OSSL_IMPLEMENT_GETAUXVAL
#  endif
# endif
# if defined(__FreeBSD__)
#  include <sys/param.h>
#  if __FreeBSD_version >= 1200000
#   include <sys/auxv.h>
#   define OSSL_IMPLEMENT_GETAUXVAL

static unsigned long getauxval(unsigned long key)
{
  unsigned long val = 0ul;

  if (elf_aux_info((int)key, &val, sizeof(val)) != 0)
    return 0ul;

  return val;
}
#  endif
# endif

/*
 * Android: according to https://developer.android.com/ndk/guides/cpu-features,
 * getauxval is supported starting with API level 18
 */
#  if defined(__ANDROID__) && defined(__ANDROID_API__) && __ANDROID_API__ >= 18
#   include <sys/auxv.h>
#   define OSSL_IMPLEMENT_GETAUXVAL
#  endif

/*
 * ARM puts the feature bits for Crypto Extensions in AT_HWCAP2, whereas
 * AArch64 used AT_HWCAP.
 */
# ifndef AT_HWCAP
#  define AT_HWCAP               16
# endif
# ifndef AT_HWCAP2
#  define AT_HWCAP2              26
# endif
# if defined(__arm__) || defined (__arm)
#  define HWCAP                  AT_HWCAP
#  define HWCAP_NEON             (1 << 12)

#  define HWCAP_CE               AT_HWCAP2
#  define HWCAP_CE_AES           (1 << 0)
#  define HWCAP_CE_PMULL         (1 << 1)
#  define HWCAP_CE_SHA1          (1 << 2)
#  define HWCAP_CE_SHA256        (1 << 3)
# elif defined(__aarch64__)
#  define HWCAP                  AT_HWCAP
#  define HWCAP_NEON             (1 << 1)

#  define HWCAP_CE               HWCAP
#  define HWCAP_CE_AES           (1 << 3)
#  define HWCAP_CE_PMULL         (1 << 4)
#  define HWCAP_CE_SHA1          (1 << 5)
#  define HWCAP_CE_SHA256        (1 << 6)
#  define HWCAP_CPUID            (1 << 11)
#  define HWCAP_CE_SHA512        (1 << 21)
# endif

# ifdef __linux__

#  ifndef OSSL_IMPLEMENT_GETAUXVAL
#   define getauxval getauxval_proc
#   define OSSL_IMPLEMENT_GETAUXVAL
#   define OSSL_IMPLEMENT_GETAUXVAL_USING_PROC
#  endif

typedef struct {
    const char *data;
    size_t len;
} STRING_PIECE;

static int STRING_PIECE_equals(const STRING_PIECE *a, const char *b)
{
    size_t b_len = strlen(b);
    return a->len == b_len && memcmp(a->data, b, b_len) == 0;
}

// STRING_PIECE_split finds the first occurence of |sep| in |in| and, if found,
// sets |*out_left| and |*out_right| to |in| split before and after it. It
// returns one if |sep| was found and zero otherwise.
static int STRING_PIECE_split(STRING_PIECE *out_left, STRING_PIECE *out_right,
                              const STRING_PIECE *in, char sep)
{
    const char *p = memchr(in->data, sep, in->len);
    if (p == NULL)
        return 0;
    // |out_left| or |out_right| may alias |in|, so make a copy.
    STRING_PIECE in_copy = *in;
    out_left->data = in_copy.data;
    out_left->len = p - in_copy.data;
    out_right->data = in_copy.data + out_left->len + 1;
    out_right->len = in_copy.len - out_left->len - 1;
    return 1;
}

// STRING_PIECE_get_delimited reads a |sep|-delimited entry from |s|, writing it
// to |out| and updating |s| to point beyond it. It returns one on success and
// zero if |s| is empty. If |s| is has no copies of |sep| and is non-empty, it
// reads the entire string to |out|.
static int STRING_PIECE_get_delimited(STRING_PIECE *s, STRING_PIECE *out, char sep)
{
    if (s->len == 0)
        return 0;
    if (!STRING_PIECE_split(out, s, s, sep)) {
        // |s| had no instances of |sep|. Return the entire string.
        *out = *s;
        s->data += s->len;
        s->len = 0;
    }
    return 1;
}

// STRING_PIECE_trim removes leading and trailing whitespace from |s|.
static void STRING_PIECE_trim(STRING_PIECE *s)
{
    while (s->len != 0 && (s->data[0] == ' ' || s->data[0] == '\t')) {
        s->data++;
        s->len--;
    }
    while (s->len != 0 &&
           (s->data[s->len - 1] == ' ' || s->data[s->len - 1] == '\t')) {
        s->len--;
    }
}

static int open_eintr(const char *path, int flags)
{
    int ret;
    do {
        ret = open(path, flags);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static ssize_t read_eintr(int fd, void *out, size_t len)
{
    ssize_t ret;
    do {
        ret = read(fd, out, len);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

#  ifdef OSSL_IMPLEMENT_GETAUXVAL_USING_PROC
// read_full reads exactly |len| bytes from |fd| to |out|. On error or end of
// file, it returns zero.
static int read_full(int fd, void *out, size_t len)
{
    char *outp = out;
    while (len > 0) {
        ssize_t ret = read_eintr(fd, outp, len);
        if (ret <= 0)
            return 0;
        outp += ret;
        len -= ret;
    }
    return 1;
}
#  endif

// read_file opens |path| and reads until end-of-file. On success, it returns
// one and sets |*out_ptr| and |*out_len| to a newly-allocated buffer with the
// contents. Otherwise, it returns zero.
static int read_file(char **out_ptr, size_t *out_len, const char *path)
{
    int fd = open_eintr(path, O_RDONLY);
    if (fd < 0)
        return 0;

    static const size_t kReadSize = 1024;
    int ret = 0;
    size_t cap = kReadSize, len = 0;
    char *buf = OPENSSL_malloc(cap);
    if (buf == NULL)
        goto err;

    for (;;) {
      if (cap - len < kReadSize) {
        size_t new_cap = cap * 2;
        if (new_cap < cap)
            goto err;
        char *new_buf = OPENSSL_realloc(buf, new_cap);
        if (new_buf == NULL)
            goto err;
        buf = new_buf;
        cap = new_cap;
      }

      ssize_t bytes_read = read_eintr(fd, buf + len, kReadSize);
      if (bytes_read < 0)
          goto err;
      if (bytes_read == 0)
          break;
      len += bytes_read;
    }

    *out_ptr = buf;
    *out_len = len;
    ret = 1;
    buf = NULL;

err:
    OPENSSL_free(buf);
    close(fd);

    return ret;
}

#  ifdef OSSL_IMPLEMENT_GETAUXVAL_USING_PROC
// getauxval_proc behaves like |getauxval| but reads from /proc/self/auxv.
static unsigned long getauxval_proc(unsigned long type)
{
    int fd = open_eintr("/proc/self/auxv", O_RDONLY);
    if (fd < 0)
        return 0;

    struct {
        unsigned long tag;
        unsigned long value;
    } entry;

    for (;;) {
        if (!read_full(fd, &entry, sizeof(entry)) ||
            (entry.tag == 0 && entry.value == 0)) {
            break;
        }
        if (entry.tag == type) {
            close(fd);
            return entry.value;
        }
    }

    close(fd);

    return 0;
}
#  endif

// extract_cpuinfo_field extracts a /proc/cpuinfo field named |field| from
// |in|. If found, it sets |*out| to the value and returns one. Otherwise, it
// returns zero.
static int extract_cpuinfo_field(STRING_PIECE *out, const STRING_PIECE *in,
                                 const char *field)
{
    // Process |in| one line at a time.
    STRING_PIECE remaining = *in, line;
    while (STRING_PIECE_get_delimited(&remaining, &line, '\n')) {
        STRING_PIECE key, value;

        if (!STRING_PIECE_split(&key, &value, &line, ':'))
            continue;

        STRING_PIECE_trim(&key);
        if (STRING_PIECE_equals(&key, field)) {
            STRING_PIECE_trim(&value);
            *out = value;
            return 1;
        }
    }

    return 0;
}

static int cpuinfo_field_equals(const STRING_PIECE *cpuinfo, const char *field,
                                const char *value)
{
    STRING_PIECE extracted;
    return extract_cpuinfo_field(&extracted, cpuinfo, field) &&
        STRING_PIECE_equals(&extracted, value);
}

// has_list_item treats |list| as a space-separated list of items and returns
// one if |item| is contained in |list| and zero otherwise.
static int has_list_item(const STRING_PIECE *list, const char *item)
{
    STRING_PIECE remaining = *list, feature;
    while (STRING_PIECE_get_delimited(&remaining, &feature, ' ')) {
        if (STRING_PIECE_equals(&feature, item))
            return 1;
    }
    return 0;
}

typedef struct {
    int loaded;
    char *data;
    STRING_PIECE str;
} CPUINFO;

#define CPUINFO_INIT { 0, }

static void CPUINFO_destroy(CPUINFO *self)
{
    OPENSSL_free(self->data);
}

static void CPUINFO_ensure_loaded(CPUINFO *self)
{
    if (self->loaded)
        return;

    // We ignore the return value of |read_file| and proceed with an empty
    // /proc/cpuinfo on error. If |getauxval| works, we will still detect
    // capabilities. There may be a false positive due to
    // |crypto_cpuinfo_has_broken_neon|, but this is now rare.
    size_t cpuinfo_len = 0;
    read_file(&self->data, &cpuinfo_len, "/proc/cpuinfo");
    self->str.data = self->data;
    self->str.len = cpuinfo_len;

    self->loaded = 1;
}

// crypto_get_arm_hwcap_from_cpuinfo returns an equivalent ARM |AT_HWCAP| value
// from |cpuinfo|.
static unsigned long crypto_get_arm_hwcap_from_cpuinfo(CPUINFO *self)
{
    const STRING_PIECE *cpuinfo = &self->str;

    CPUINFO_ensure_loaded(self);

    if (cpuinfo_field_equals(cpuinfo, "CPU architecture", "8")) {
        // This is a 32-bit ARM binary running on a 64-bit kernel. NEON is
        // always available on ARMv8. Linux omits required features, so reading
        // the "Features" line does not work. (For simplicity, use strict
        // equality. We assume everything running on future ARM architectures
        // will have a working |getauxval|.)
        return HWCAP_NEON;
    }

    STRING_PIECE features;
    if (extract_cpuinfo_field(&features, cpuinfo, "Features") &&
            has_list_item(&features, "neon")) {
        return HWCAP_NEON;
    }

    return 0;
}

// crypto_get_arm_hwcap2_from_cpuinfo returns an equivalent ARM |AT_HWCAP2|
// value from |cpuinfo|.
static unsigned long crypto_get_arm_hwcap2_from_cpuinfo(CPUINFO *self)
{
    const STRING_PIECE *cpuinfo = &self->str;

    CPUINFO_ensure_loaded(self);

    STRING_PIECE features;
    if (!extract_cpuinfo_field(&features, cpuinfo, "Features"))
        return 0;

    unsigned long ret = 0;

    if (has_list_item(&features, "aes"))
        ret |= HWCAP_CE_AES;

    if (has_list_item(&features, "pmull"))
        ret |= HWCAP_CE_PMULL;

    if (has_list_item(&features, "sha1"))
        ret |= HWCAP_CE_SHA1;

    if (has_list_item(&features, "sha2"))
        ret |= HWCAP_CE_SHA256;

    return ret;
}

#  if defined(__arm__) || defined (__arm)

// crypto_cpuinfo_has_broken_neon returns one if |cpuinfo| matches a CPU known
// to have broken NEON unit and zero otherwise. See https://crbug.com/341598.
static int crypto_cpuinfo_has_broken_neon(const STRING_PIECE *cpuinfo)
{
    return cpuinfo_field_equals(cpuinfo, "CPU implementer", "0x51") &&
        cpuinfo_field_equals(cpuinfo, "CPU architecture", "7") &&
        cpuinfo_field_equals(cpuinfo, "CPU variant", "0x1") &&
        cpuinfo_field_equals(cpuinfo, "CPU part", "0x04d") &&
        cpuinfo_field_equals(cpuinfo, "CPU revision", "0");
}

#  endif
# endif /* __linux__ */

# ifndef OSSL_IMPLEMENT_GETAUXVAL
static sigset_t all_masked;

static sigjmp_buf ill_jmp;
static void ill_handler(int sig)
{
    siglongjmp(ill_jmp, sig);
}
# endif

void OPENSSL_cpuid_setup(void)
{
    const char *e;
# ifndef OSSL_IMPLEMENT_GETAUXVAL
    struct sigaction ill_oact, ill_act;
    sigset_t oset;
#endif
    static int trigger = 0;

    if (trigger)
        return;
    trigger = 1;

    OPENSSL_armcap_P = 0;

    if ((e = getenv("OPENSSL_armcap"))) {
        OPENSSL_armcap_P = (unsigned int)strtoul(e, NULL, 0);
        return;
    }

# if defined(__APPLE__)
#   if !defined(__aarch64__)
    /*
     * Capability probing by catching SIGILL appears to be problematic
     * on iOS. But since Apple universe is "monocultural", it's actually
     * possible to simply set pre-defined processor capability mask.
     */
    if (1) {
        OPENSSL_armcap_P = ARMV7_NEON;
        return;
    }
    /*
     * One could do same even for __aarch64__ iOS builds. It's not done
     * exclusively for reasons of keeping code unified across platforms.
     * Unified code works because it never triggers SIGILL on Apple
     * devices...
     */
#   else
    {
        unsigned int sha512;
        size_t len = sizeof(sha512);

        if (sysctlbyname("hw.optional.armv8_2_sha512", &sha512, &len, NULL, 0) == 0 && sha512 == 1)
            OPENSSL_armcap_P |= ARMV8_SHA512;
    }
#   endif
# endif

# ifdef OSSL_IMPLEMENT_GETAUXVAL
    unsigned long hwcap1 = getauxval(HWCAP);

#  ifdef __linux__
    CPUINFO cpuinfo = CPUINFO_INIT;

    // Turns out /proc/self/auxv is unreadable on some versions of Android, so
    // fall back to /proc/cpuinfo.
    //
    // See
    // https://android.googlesource.com/platform/ndk/+/882ac8f3392858991a0e1af33b4b7387ec856bd2
    // and b/13679666 (Google-internal) for details.
    if (hwcap1 == 0)
        hwcap1 = crypto_get_arm_hwcap_from_cpuinfo(&cpuinfo);

#   if defined(__arm__) || defined (__arm)
    if (crypto_cpuinfo_has_broken_neon(&cpuinfo))
        hwcap1 &= ~HWCAP_NEON;
#   endif
#  endif

    if (hwcap1 & HWCAP_NEON) {
        unsigned long hwcap;
        if (HWCAP_CE != HWCAP) {
            hwcap = getauxval(HWCAP_CE);
#  ifdef __linux__
            // Some ARMv8 Android devices don't expose HWCAP2. Fall back to
            // /proc/cpuinfo. See https://crbug.com/596156.
            if (hwcap == 0)
                hwcap = crypto_get_arm_hwcap2_from_cpuinfo(&cpuinfo);
#  endif
        } else {
            hwcap = hwcap1;
        }

        OPENSSL_armcap_P |= ARMV7_NEON;

        if (hwcap & HWCAP_CE_AES)
            OPENSSL_armcap_P |= ARMV8_AES;

        if (hwcap & HWCAP_CE_PMULL)
            OPENSSL_armcap_P |= ARMV8_PMULL;

        if (hwcap & HWCAP_CE_SHA1)
            OPENSSL_armcap_P |= ARMV8_SHA1;

        if (hwcap & HWCAP_CE_SHA256)
            OPENSSL_armcap_P |= ARMV8_SHA256;

#  ifdef __aarch64__
        if (hwcap & HWCAP_CE_SHA512)
            OPENSSL_armcap_P |= ARMV8_SHA512;

        if (hwcap & HWCAP_CPUID)
            OPENSSL_armcap_P |= ARMV8_CPUID;
#  endif
    }

#  ifdef __linux__
    CPUINFO_destroy(&cpuinfo);
#  endif
# endif

# ifndef OSSL_IMPLEMENT_GETAUXVAL
    sigfillset(&all_masked);
    sigdelset(&all_masked, SIGILL);
    sigdelset(&all_masked, SIGTRAP);
    sigdelset(&all_masked, SIGFPE);
    sigdelset(&all_masked, SIGBUS);
    sigdelset(&all_masked, SIGSEGV);

    memset(&ill_act, 0, sizeof(ill_act));
    ill_act.sa_handler = ill_handler;
    ill_act.sa_mask = all_masked;

    sigprocmask(SIG_SETMASK, &ill_act.sa_mask, &oset);
    sigaction(SIGILL, &ill_act, &ill_oact);

    /* If we used getauxval, we already have all the values */
    if (sigsetjmp(ill_jmp, 1) == 0) {
        _armv7_neon_probe();
        OPENSSL_armcap_P |= ARMV7_NEON;
        if (sigsetjmp(ill_jmp, 1) == 0) {
            _armv8_pmull_probe();
            OPENSSL_armcap_P |= ARMV8_PMULL | ARMV8_AES;
        } else if (sigsetjmp(ill_jmp, 1) == 0) {
            _armv8_aes_probe();
            OPENSSL_armcap_P |= ARMV8_AES;
        }
        if (sigsetjmp(ill_jmp, 1) == 0) {
            _armv8_sha1_probe();
            OPENSSL_armcap_P |= ARMV8_SHA1;
        }
        if (sigsetjmp(ill_jmp, 1) == 0) {
            _armv8_sha256_probe();
            OPENSSL_armcap_P |= ARMV8_SHA256;
        }
#  if defined(__aarch64__) && !defined(__APPLE__)
        if (sigsetjmp(ill_jmp, 1) == 0) {
            _armv8_sha512_probe();
            OPENSSL_armcap_P |= ARMV8_SHA512;
        }
#  endif
    }

    /*
     * Probing for ARMV7_TICK is known to produce unreliable results,
     * so we will only use the feature when the user explicitly enables
     * it with OPENSSL_armcap.
     */

    sigaction(SIGILL, &ill_oact, NULL);
    sigprocmask(SIG_SETMASK, &oset, NULL);
# endif

# ifdef __aarch64__
    if (OPENSSL_armcap_P & ARMV8_CPUID)
        OPENSSL_arm_midr = _armv8_cpuid_probe();

    if ((MIDR_IS_CPU_MODEL(OPENSSL_arm_midr, ARM_CPU_IMP_ARM, ARM_CPU_PART_CORTEX_A72) ||
         MIDR_IS_CPU_MODEL(OPENSSL_arm_midr, ARM_CPU_IMP_ARM, ARM_CPU_PART_N1)) &&
        (OPENSSL_armcap_P & ARMV7_NEON)) {
            OPENSSL_armv8_rsa_neonized = 1;
    }
# endif
}
#endif
