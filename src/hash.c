/**
 * @file hash.c
 *
 * SHA-256 wrapper used by the benchmark.
 *
 * This file also holds the one copy of the SHA-256 implementation.
 * The LONESHA256_IMPLEMENTATION macro tells the header to add the function body here.
 */

#define LONESHA256_IMPLEMENTATION
#include "../vendor/lonesha256.h"

#include "hash.h"
#include <string.h>

/**
 * @function write_number_little_endian
 * @brief Put the 8 bytes of a number in a buffer.
 *
 * The bytes go in little-endian order, so the least significant byte is first.
 * @param value The number to write.
 * @param buffer The output buffer that will hold the 8 bytes.
 */
static void write_number_little_endian(uint64_t value, uint8_t buffer[8])
{
    for (int i = 0; i < 8; i++)
    {
        buffer[i] = (uint8_t)(value & 0xFF);
        value >>= 8;
    }
}

/**
 * @function hash_of_number
 * @brief Make the SHA-256 hash of a 64-bit number.
 *
 * The value is written to the output buffer as 32 bytes.
 * @param value The number to hash.
 * @param out The buffer for the result.
 */
void hash_of_number(uint64_t value, uint8_t out[HASH_SIZE])
{
    uint8_t input[8];

    // Step 1: Make the 8-byte input in the correct byte order.
    write_number_little_endian(value, input);

    // Step 2: Make the SHA-256 hash of these 8 bytes.
    lonesha256(out, input, sizeof(input));
}

/**
 * @function hash_equal
 * @brief Compare two hashes.
 *
 * This is a simple wrapper around memcmp.
 * @param a The first hash to compare.
 * @param b The second hash to compare.
 * @return 1 if the hashes are equal, otherwise 0.
 */
int hash_equal(const uint8_t a[HASH_SIZE], const uint8_t b[HASH_SIZE])
{
    // memcmp returns 0 when the two buffers are equal.
    return memcmp(a, b, HASH_SIZE) == 0;
}
