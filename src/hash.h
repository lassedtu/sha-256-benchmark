/**
 * @file hash.h
 *
 * Simple wrapper around the provided SHA-256 hasher.
 *
 * This file gives a small interface for making the SHA-256 hash of a 64-bit value.
 */

#ifndef HASH_H
#define HASH_H

#include <stdint.h>
#include <stddef.h>

// A SHA-256 hash has 256 bits. This is 32 bytes.
#define HASH_SIZE 32

/**
 * @function hash_of_number
 * @brief Make the SHA-256 hash of a 64-bit number.
 *
 * @param value The number to hash.
 * @param out The output buffer. It must hold 32 bytes.
 */
void hash_of_number(uint64_t value, uint8_t out[HASH_SIZE]);

/**
 * @function hash_equal
 * @brief Compare two hashes.
 *
 * @param a The first hash.
 * @param b The second hash.
 * @return 1 if the hashes are equal, otherwise 0.
 */
int hash_equal(const uint8_t a[HASH_SIZE], const uint8_t b[HASH_SIZE]);

#endif // HASH_H
