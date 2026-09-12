// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file sha256.h
/// SHA-256 (FIPS 180-4), vendored.
///
/// **Why a copy rather than a library.** This is the only cryptographic primitive projectMM needs,
/// and it needs it on all five targets. OpenSSL does not exist on ESP32 (ESP-IDF ships mbedtls), so
/// linking it would mean an mbedtls path for devices and an OpenSSL path for the desktop: two
/// implementations of one function that must agree byte for byte, where a divergence produces
/// identifiers that silently do not match between platforms. One vendored file is the smaller
/// thing to own, and the algorithm is frozen: FIPS 180-4 has not changed since 2015 and the test
/// vectors are published, so `unit_sha256.cpp` pins this against them.
///
/// The one use today is the MoonStats installation id
/// ([the MoonCloud plan](../../docs/work/present/Plan-20260910 - MoonCloud.md)). It is NOT a
/// general-purpose crypto layer: no HMAC, no streaming over a socket, no constant-time comparison,
/// because nothing here needs them and an unused primitive is a maintenance cost with no user.

#include <cstddef>
#include <cstdint>

namespace mm {

/// Digest length in bytes.
inline constexpr size_t kSha256DigestSize = 32;

/// Hash `len` bytes at `data` into `out`, which must hold 32 bytes.
///
/// One shot rather than init/update/final: every caller here hashes a short buffer that is already
/// in memory, and the streaming form would be three functions nobody calls with more than one
/// update.
void sha256(const void* data, size_t len, uint8_t out[kSha256DigestSize]);

/// Hash `len` bytes and write the first `outBytes` of the digest as lowercase hex into `out`.
///
/// `out` must hold `outBytes * 2 + 1` characters. Truncating a SHA-256 is the standard way to get a
/// shorter identifier (NIST SP 800-107 §5.1); 16 bytes leaves 128 bits, far past any collision
/// concern for a population of LED controllers.
void sha256Hex(const void* data, size_t len, char* out, size_t outBytes = 16);

}  // namespace mm
