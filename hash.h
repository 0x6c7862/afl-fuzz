/*
   american fuzzy lop - hashing function
   -------------------------------------

   The hash32() function is a variant of MurmurHash3, a good
   non-cryptosafe hashing function developed by Austin Appleby.

   For simplicity, this variant does *NOT* accept buffer lengths
   that are not divisible by 8 bytes. The 32-bit version is otherwise
   similar to the original; the 64-bit one is a custom hack with
   mostly-unproven properties.

   Austin's original code is public domain; so is this variant.

 */

#ifndef _HAVE_HASH_H
#define _HAVE_HASH_H

#include "types.h"

#ifdef __x86_64__

#define ROL64(_x, _r)  ((((u64)(_x)) << (_r)) | (((u64)(_x)) >> (64 - (_r))))

static inline u32 hash32(const void* key, u32 len, u32 seed) {

  const u64* data = (u64*)key;
  u64 h1 = seed ^ len;

  len >>= 3;

  while (len--) {

    u64 k1 = *data++;

    k1 *= 0x87c37b91114253d5ULL;
    k1  = ROL64(k1, 31);
    k1 *= 0x4cf5ad432745937fULL;

    h1 ^= k1;
    h1  = ROL64(h1, 27);
    h1  = h1 * 5 + 0x52dce729;

  }

  h1 ^