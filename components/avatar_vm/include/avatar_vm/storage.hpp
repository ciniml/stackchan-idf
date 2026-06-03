// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <tl/expected.hpp>

#include "avatar_vm/bytecode.hpp"

// Persists a user-uploaded avatar face bytecode in NVS (key "face_bc" in the
// "avatar_vm" namespace). Caller boots, asks for the saved blob; if present
// and decodes cleanly, hands it to Avatar::load_face_bytecode; otherwise the
// firmware-embedded default (default_face.avbc) is used.

namespace stackchan::avatar_vm::storage {

enum class StorageError : std::uint8_t {
    NvsOpen = 1,
    NotFound,
    ReadFailed,
    WriteFailed,
    CommitFailed,
    EraseFailed,
    TooLarge,
};

const char* to_string(StorageError e) noexcept;

// Hard cap on the bytecode size we'll accept. Anything bigger is rejected at
// the upload boundary (HTTP / BLE) and never written to NVS. 32 KiB is well
// beyond a hand-authored face (~1.5 KB) but leaves headroom for elaborate
// user creations without burning the NVS partition.
inline constexpr std::size_t kMaxBytecodeBytes = 32 * 1024;

// Read the saved bytecode from NVS. Returns NotFound when no override exists
// (caller should fall back to the embedded default).
tl::expected<std::vector<std::uint8_t>, StorageError> load() noexcept;

// Validate + save. Returns TooLarge if `bytes.size() > kMaxBytecodeBytes`.
// The bytecode is validated via decode() before writing so we never persist a
// blob that would fail to boot. Decode failures map to WriteFailed.
tl::expected<void, StorageError> save(std::span<const std::uint8_t> bytes) noexcept;

// Forget any saved override (next boot uses the embedded default).
tl::expected<void, StorageError> clear() noexcept;

} // namespace stackchan::avatar_vm::storage
