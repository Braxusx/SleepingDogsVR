#pragma once

#include "Config.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace spatch::pool_headroom {

// The engine's Main Pool is created once, early, by the game. Its initial
// reservation is a single VirtualAlloc of 500 MiB (0x1F400000) made by the
// pool-create wrapper at rva 0x426F70 in the legacy_researched layout. When
// a request does not fit in the pool's own space, the allocator reserves a
// fresh region sized to that request and commits it (rva 0x166BA3, via the
// helpers at rva 0xA3A500 and 0xA3A520). When the pool can serve no
// request and cannot grow, the allocator reports "out of memory" and the game
// traps.
//
// Two immediates in that wrapper carry the reservation size:
//   0x426F74  b9 00 00 40 1f    mov ecx, 0x1F400000  (size for VirtualAlloc)
//   0x426FAD  41 b9 00 00 40 1f mov r9d, 0x1F400000 (size recorded in the pool)
// Both must be rewritten to the same value, or the pool would reserve one
// size and record another. They sit 58 bytes apart, so a single patch
// transaction covers both and the reservation can never be half-applied.
//
// The patch window starts at the first immediate's operand, not at the
// function entry:
//   0x426F70  48 83 ec 58     sub rsp, 0x58
//   0x426F74  b9 ...          mov ecx, imm32
//   0x426F75  ....            <- the imm32 operand, window starts here
//   ...
//   0x426FAD  41 b9           mov r9d, imm32
//   0x426FAF  ....            <- the second imm32 operand
//   0x426FB2                  <- last byte of the window
// The window is the 62 bytes from 0x426F75 to 0x426FB2. Anchoring it on
// the operand rather than the entry keeps the offsets and the base in one
// place: a base that pointed at the entry would compare the function
// prologue against the window and the patch would always be refused.
//
// The pool is created by the game, not by the mod, so the patch only lands
// if it is applied before that first pool creation.
//
// The mod is loaded by the Simple ASI Loader, which the game executable
// imports statically. The loader loads every .asi file in the game
// directory from its own DllMain, so the mod's DllMain runs while the
// loader still holds its lock: before the game's entry point and before
// any game code can create the pool. A patch written from that DllMain
// therefore always lands in time, and that is the only place where the
// race is won by construction rather than by luck.
//
// The later bootstrap thread is too late on its own. The first live run
// of this feature measured the pool already created by the time the
// bootstrap thread reached the patch, with the game's window and its
// Direct3D device still minutes away from being set up, so the pool is
// created in the game's first moments and the mod's own work (config
// load, logging, hashing the executable) was enough to miss it.
//
// ApplyPoolHeadroomInLoaderPhase writes the patch from that earlier
// point. The bootstrap path stays as the fallback for a mod that is
// loaded later, and both report what actually happened rather than
// assuming.

// Why a headroom attempt ended the way it did.
//
// The reason travels in the receipt, so the reader is told what happened
// instead of having to infer it from a set of flags.
enum class HeadroomOutcome {
    // No attempt was made.
    NotAttempted,
    // The option asked for no headroom.
    OptionDisabled,
    // There was no game executable to patch.
    NoGameModule,
    // The executable's SHA-256 could not be computed, so the build
    // could not be identified.
    HashFailed,
    // The build is not the one the pool offsets were researched
    // against.
    UnsupportedLayout,
    // The build identity was not proven by the executable's SHA-256.
    UnverifiedBuild,
    // The requested reservation is not larger than the stock one.
    NotLargerThanStock,
    // The requested reservation does not fit in the patched immediate.
    SizeDoesNotFit,
    // The patch site did not hold the expected bytes, or the write
    // failed.
    PatchRefused,
    // The patch was written, but the game had already created the pool,
    // so the patched creation size had no effect.
    WrittenAfterPoolCreation,
    // The patch was written before the game created the pool.
    Applied,
};

// The name of an outcome, for the log and the receipt.
const char* HeadroomOutcomeName(HeadroomOutcome outcome) noexcept;

struct HeadroomPatchResult {
    // The patch bytes were written.
    bool applied = false;
    // The patch site did not hold the expected stock bytes (a different
    // build, so the researched offsets do not apply).
    bool site_mismatch = false;
    // The pool already existed when the patch was attempted, so the
    // creation-time size no longer had any effect this session.
    bool pool_already_created = false;
    // The pool did not exist yet, so the patched creation size is the one
    // the game will use.
    bool pool_not_yet_created = false;
    // The layout this feature was researched against is not the one in
    // use; nothing was patched.
    bool unsupported_layout = false;
    // The reservation size that was requested, in MiB.
    int requested_mib = 0;
    // The reservation size the pool actually has, in MiB, read back from
    // the live pool object. Zero when the pool could not be read.
    int effective_mib = 0;
    // The address the patch was aimed at, when a patch was attempted.
    //
    // The address is recorded rather than only used, so a test can check
    // the address the patch code actually chose. Without that, a patch
    // code change that used a different base would still pass every test
    // that only checked the constants.
    std::uintptr_t attempted_address = 0;
    // The attempt was made from the loader phase, before the game had
    // started running. A write from there is the only one that can be
    // relied on to land before the game creates the pool.
    bool loader_phase = false;
    // The build identity was proven by the executable's SHA-256 rather
    // than by its file metadata.
    bool hash_verified = false;
    // Why the attempt ended the way it did.
    HeadroomOutcome outcome = HeadroomOutcome::NotAttempted;
    // The reservation the pool ended up with, in MiB, read back after the
    // game created it. -1 when the creation was never observed.
    int effective_mib_after_creation = -1;
    // The pool's creation was observed after the patch was written.
    bool pool_creation_observed = false;
    // Milliseconds since boot when the patch was written. Zero when the
    // attempt never reached the write.
    std::uint64_t patch_tick_ms = 0;
};

// Applies the headroom change when the option asks for one and the build
// matches. Safe to call when the option is off: it returns immediately.
//
// module_base is the game's executable base. build_id is the identity the
// build check produced, and hook_layout_verified says whether that
// identity was proven by the executable's SHA-256 rather than only by its
// file metadata.
//
// Both are required. A metadata-only match is not proof that the fixed
// RVAs mean the same thing, and a patch at a fixed RVA is exactly the
// kind of write that needs proof. Only a hash-verified
// "legacy_researched" build is patched; every other case, including
// "unknown" and an unverified "legacy_researched", is refused.
HeadroomPatchResult ApplyPoolHeadroom(const Config& config,
                                      std::uintptr_t module_base,
                                      std::string_view build_id,
                                      bool hook_layout_verified);

// Applies the headroom change from the loader phase: before the game has
// started running, while the loader still holds its lock.
//
// game_exe_path is the game's executable. The executable's SHA-256 is
// computed here and has to match the researched build, because the
// researched RVAs mean nothing on another build and a fixed-RVA write is
// exactly the kind of write that needs that proof. Hashing the
// executable costs tens of milliseconds, which the game spends starting
// up anyway.
//
// requested_mib is passed in rather than read from the configuration
// here, because the loader phase reads as little of the configuration as
// it can while the loader lock is held.
//
// The write itself goes through the same patch registry as the bootstrap
// path, so the bytes are checked against the researched window before
// they are written and the write is recorded for rollback.
HeadroomPatchResult ApplyPoolHeadroomInLoaderPhase(
    const std::filesystem::path& game_exe_path,
    std::uintptr_t module_base,
    int requested_mib);

// The pool-create wrapper in the legacy_researched layout, for reference.
inline constexpr std::uintptr_t kPoolCreateWrapperRva = 0x00426F70;

// The first byte of the patched window: the operand of the first
// immediate.
inline constexpr std::uintptr_t kPatchWindowRva = 0x00426F75;

// The patched window, described so it can be built and checked without
// touching a live process. The window starts at the first immediate's
// operand and runs through the second one's operand.
inline constexpr std::size_t kPatchWindowLength = 62;
inline constexpr std::size_t kFirstImmediateOffset = 0x00;   // mov ecx, imm32
inline constexpr std::size_t kSecondImmediateOffset = 0x3A;  // mov r9d, imm32

// The rvas of the two immediate operands, which the window offsets must
// land on exactly.
inline constexpr std::uintptr_t kFirstImmediateOperandRva = 0x00426F75;
inline constexpr std::uintptr_t kSecondImmediateOperandRva = 0x00426FAF;

// The stock bytes of that window, read out of the shipped executable.
inline constexpr std::array<std::uint8_t, kPatchWindowLength>
    kStockPoolCreateBytes{
        0x00, 0x00, 0x40, 0x1F, 0xE8, 0x52, 0x35, 0x61, 0x00, 0x48, 0x8B, 0x0D,
        0x0B, 0x12, 0xE3, 0x01, 0xC6, 0x44, 0x24, 0x48, 0x01, 0xC7, 0x44, 0x24,
        0x40, 0x01, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x38, 0x00, 0x00,
        0x00, 0x00, 0xC7, 0x44, 0x24, 0x30, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8D,
        0x15, 0xD6, 0x35, 0x36, 0x01, 0x4C, 0x8B, 0xC0, 0x41, 0xB9, 0x00, 0x00,
        0x40, 0x1F};

// The reservation the stock build makes.
inline constexpr std::uint32_t kStockReservationBytes = 0x1F400000;  // 500 MiB

// The largest value the 32-bit immediate can carry.
inline constexpr std::uint32_t kMaxReservationBytes = 0xFFFFFFFFu;

// The address the patch is written to, for a given game module base.
//
// The patch code and the tests both go through this one function, so the
// address that is written and the address that is checked can never drift
// apart. A test that only asserted the constants would still pass if the
// patch code started using a different one.
inline constexpr std::uintptr_t PatchAddress(std::uintptr_t module_base) {
    return module_base + kPatchWindowRva;
}

// Builds the byte window that replaces the stock one when a reservation of
// the given size in MiB is requested. Both immediates receive the same
// value, so the pool's reserved size and its recorded bounds always agree.
//
// Returns nothing when the size does not fit in the immediate, so a bad
// value can never be written into the executable. The result is optional
// rather than a sentinel array on purpose: a sentinel would be
// indistinguishable from a real patch, and a zeroed window written over
// the pool-create wrapper would ask the game for an empty arena.
std::optional<std::array<std::uint8_t, kPatchWindowLength>>
BuildReservationPatch(int mib);

// Writes what the headroom option did to a file, so the outcome is
// visible without turning on logging.
//
// The option is experimental and its result matters: the user has to be
// able to see whether the pool had already been created by the time the
// mod loaded, because in that case the game kept the stock reservation.
// The log is off by default, so a log line alone would leave the user
// with nothing to read.
//
// The file is written next to the game's SPatch.ini. Returns false when
// the file could not be written.
bool WritePoolHeadroomReceipt(const std::filesystem::path& path,
                               const HeadroomPatchResult& result);

}  // namespace spatch::pool_headroom
