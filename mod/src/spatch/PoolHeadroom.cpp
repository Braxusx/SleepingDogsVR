#include "PoolHeadroom.h"

#include "BuildInfo.h"
#include "Logger.h"
#include "PoolDiagnostics.h"
#include "RuntimePatch.h"
#include "VersionGuard.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace spatch::pool_headroom {
namespace {

// One registry instance owns this file's patches. The write path
// serializes against every other executable write in the process through
// the shared transaction mutex in RuntimePatch.cpp.
//
// The instance is a function-local static on purpose. The loader phase
// reaches this code from DllMain, and a namespace-scope instance would
// depend on the C++ static initializers having run before the module's
// DllMain is called. A function-local static is initialized on first use,
// so it does not depend on that order at all.
runtime_patch::Registry& PoolHeadroomPatches() {
    static runtime_patch::Registry registry;
    return registry;
}

// The shared part of both attempts.
//
// The loader phase and the bootstrap thread come through here, so the two
// paths cannot drift apart in what they check, in what they write, and in
// what they report.
HeadroomPatchResult ApplyHeadroom(int requested_mib,
                                  std::uintptr_t module_base,
                                  bool build_is_legacy_researched,
                                  bool hash_verified,
                                  bool loader_phase) {
    HeadroomPatchResult result{};
    result.requested_mib = requested_mib;
    result.loader_phase = loader_phase;
    result.hash_verified = hash_verified;

    // Every exit reports the same summary, so the log always says what the
    // pool really has alongside what the option did.
    const auto finish = [&result](HeadroomOutcome outcome) {
        result.outcome = outcome;
        log::InfoF(
            "pool_headroom requested_mib=%d applied=%d pool_existed=%d "
            "effective_reservation_mib=%d loader_phase=%d reason=%s",
            result.requested_mib,
            result.applied ? 1 : 0,
            result.pool_already_created ? 1 : 0,
            result.effective_mib,
            result.loader_phase ? 1 : 0,
            HeadroomOutcomeName(outcome));
        return result;
    };

    if (requested_mib <= 0) {
        // The option is off. The summary is still written so the log says
        // the option was not in effect, rather than leaving the reader to
        // work that out from its absence.
        return finish(HeadroomOutcome::OptionDisabled);
    }
    if (module_base == 0) {
        // There is no game to patch.
        return finish(HeadroomOutcome::NoGameModule);
    }

    // The live pool is read before any refusal, so every path out of this
    // function can report what the pool actually has. The user asked for
    // the option to say what it did, and a refusal that only names the
    // reason would leave the useful half unsaid.
    const bool pool_exists = pool_diagnostics::PoolExists(module_base);
    result.pool_already_created = pool_exists;
    result.pool_not_yet_created = !pool_exists;
    pool_diagnostics::PoolState state;
    if (pool_diagnostics::ReadPoolState(module_base, &state) &&
        state.segment_bytes != 0) {
        result.effective_mib =
            static_cast<int>(state.segment_bytes / (1024ull * 1024ull));
    } else {
        // The pool does not exist yet, so there is no reservation to
        // report. Zero would read as "the pool reserved nothing", which is
        // a different claim from "not known yet".
        result.effective_mib = -1;
    }

    // Only the legacy_researched layout has the pool RVAs this feature was
    // researched against. Every other identity, including "unknown", is
    // refused: the patch would land on bytes that mean something else.
    if (!build_is_legacy_researched) {
        result.unsupported_layout = true;
        log::WarnF(
            "pool_headroom requested but the build is not the researched "
            "layout; headroom disabled");
        return finish(HeadroomOutcome::UnsupportedLayout);
    }

    // The identity also has to have been proven by the executable's
    // SHA-256. A file-metadata match alone does not show that the bytes
    // at the researched RVAs are the bytes this feature was researched
    // against, and a patch at a fixed RVA is exactly the write that needs
    // that proof.
    if (!hash_verified) {
        result.unsupported_layout = true;
        log::Warn(
            "pool_headroom requested but the game build was not verified by "
            "hash; headroom disabled");
        return finish(HeadroomOutcome::UnverifiedBuild);
    }

    const std::uint64_t requested_bytes =
        static_cast<std::uint64_t>(requested_mib) * 1024ull * 1024ull;
    if (requested_bytes <= kStockReservationBytes) {
        log::WarnF(
            "pool_headroom requested=%d MiB is not larger than the stock "
            "500 MiB reservation; headroom disabled",
            requested_mib);
        return finish(HeadroomOutcome::NotLargerThanStock);
    }

    const auto replacement = BuildReservationPatch(requested_mib);
    if (!replacement.has_value()) {
        // The size does not fit in the patched immediate. Nothing is
        // written, so the game keeps the stock reservation. The size is
        // checked in 64 bits here and narrowed only once it is known to
        // fit, so a value that does not fit is refused rather than
        // truncated to a different size.
        log::WarnF(
            "pool_headroom requested=%d MiB does not fit in the patch "
            "immediate; headroom disabled",
            requested_mib);
        return finish(HeadroomOutcome::SizeDoesNotFit);
    }

    {
        // The window is anchored on the first immediate's operand, so the
        // bytes the runtime patch expects are exactly the bytes at that
        // address in the shipped executable. The address comes from the
        // shared helper, so the patch and the tests cannot disagree
        // about where the window starts.
        const std::uintptr_t address = PatchAddress(module_base);
        result.attempted_address = address;
        const auto outcome = PoolHeadroomPatches().Apply(
            "pool_headroom_reservation",
            address,
            kStockPoolCreateBytes,
            *replacement);
        result.applied = outcome == runtime_patch::ApplyResult::Applied ||
                         outcome == runtime_patch::ApplyResult::AlreadyApplied;
        if (!result.applied) {
            result.site_mismatch =
                outcome == runtime_patch::ApplyResult::UnexpectedBytes;
            log::WarnF(
                "pool_headroom requested=%d MiB not applied (patch result %s)",
                requested_mib,
                runtime_patch::ApplyResultName(outcome));
            return finish(HeadroomOutcome::PatchRefused);
        }
    }
    result.patch_tick_ms = GetTickCount64();

    // The bytes were written, but the game creates the pool itself. If
    // the pool already existed, the patched creation size has no effect
    // this session: the game keeps the stock reservation. The outcome
    // says so, so "applied=1" is never read as "the change took effect".
    if (result.pool_already_created) {
        log::Warn(
            "pool_headroom was written but the pool had already been "
            "created, so the game keeps the stock reservation this session");
        return finish(HeadroomOutcome::WrittenAfterPoolCreation);
    }

    return finish(HeadroomOutcome::Applied);
}

// The receipt's explanation of what happened, in words.
//
// The explanation has to match what the option actually did. A patch that
// was written into a running game is not a change that carries over to
// the next launch: the bytes live in that process's memory, so the next
// launch starts from the stock reservation again and the patch has to land
// in time once more.
std::string OutcomeProse(const HeadroomPatchResult& result) {
    switch (result.outcome) {
        case HeadroomOutcome::Applied:
            return "The change was applied before the game created the "
                   "pool, so the game created its Main Pool with the "
                   "larger reservation.";
        case HeadroomOutcome::WrittenAfterPoolCreation:
            return "The pool had already been created when the patch was "
                   "written, so the game kept its own reservation this "
                   "session. The patch is not saved to disk: it is written "
                   "again on every launch, and it only has an effect when "
                   "it is written before the game creates the pool.";
        case HeadroomOutcome::UnsupportedLayout:
            return "This build is not the one the pool offsets were "
                   "researched against, so nothing was changed.";
        case HeadroomOutcome::UnverifiedBuild:
            return "The build identity was not proven by the game "
                   "executable's SHA-256, so nothing was changed.";
        case HeadroomOutcome::HashFailed:
            return "The game executable's SHA-256 could not be computed, so "
                   "the build could not be identified and nothing was "
                   "changed.";
        case HeadroomOutcome::NotLargerThanStock:
            return "The requested reservation is not larger than the stock "
                   "500 MiB reservation, so nothing was changed.";
        case HeadroomOutcome::SizeDoesNotFit:
            return "The requested reservation does not fit in the value the "
                   "game's pool creation code can hold, so nothing was "
                   "changed.";
        case HeadroomOutcome::PatchRefused:
            return "The patch was refused because the bytes at the pool "
                   "creation site did not match the build this feature was "
                   "researched against, so nothing was changed.";
        case HeadroomOutcome::OptionDisabled:
            return "The headroom option is off, so nothing was changed.";
        case HeadroomOutcome::NoGameModule:
            return "There was no game executable to patch, so nothing was "
                   "changed.";
        case HeadroomOutcome::NotAttempted:
            break;
    }
    return "Nothing was changed. See the log for the reason.";
}

}  // namespace

const char* HeadroomOutcomeName(HeadroomOutcome outcome) noexcept {
    switch (outcome) {
        case HeadroomOutcome::NotAttempted:
            return "not_attempted";
        case HeadroomOutcome::OptionDisabled:
            return "option_disabled";
        case HeadroomOutcome::NoGameModule:
            return "no_game_module";
        case HeadroomOutcome::HashFailed:
            return "hash_failed";
        case HeadroomOutcome::UnsupportedLayout:
            return "unsupported_layout";
        case HeadroomOutcome::UnverifiedBuild:
            return "unverified_build";
        case HeadroomOutcome::NotLargerThanStock:
            return "not_larger_than_stock";
        case HeadroomOutcome::SizeDoesNotFit:
            return "size_does_not_fit";
        case HeadroomOutcome::PatchRefused:
            return "patch_refused";
        case HeadroomOutcome::WrittenAfterPoolCreation:
            return "written_after_pool_creation";
        case HeadroomOutcome::Applied:
            return "applied";
    }
    return "unknown";
}

std::optional<std::array<std::uint8_t, kPatchWindowLength>>
BuildReservationPatch(int mib) {
    if (mib <= 0) {
        return std::nullopt;
    }

    // The size is computed in 64 bits and checked before it is narrowed,
    // so a value that does not fit in the 32-bit immediate is refused
    // rather than truncated to a different size.
    const std::uint64_t size =
        static_cast<std::uint64_t>(mib) * 1024ull * 1024ull;
    if (size == 0 || size > kMaxReservationBytes) {
        return std::nullopt;
    }

    std::array<std::uint8_t, kPatchWindowLength> bytes = kStockPoolCreateBytes;
    const std::uint32_t size_le = static_cast<std::uint32_t>(size);
    std::memcpy(bytes.data() + kFirstImmediateOffset, &size_le, sizeof(size_le));
    std::memcpy(bytes.data() + kSecondImmediateOffset, &size_le, sizeof(size_le));
    return bytes;
}

HeadroomPatchResult ApplyPoolHeadroom(const Config& config,
                                      std::uintptr_t module_base,
                                      std::string_view build_id,
                                      bool hook_layout_verified) {
    return ApplyHeadroom(config.pool_headroom_mib,
                         module_base,
                         build_id == "legacy_researched",
                         hook_layout_verified,
                         false);
}

HeadroomPatchResult ApplyPoolHeadroomInLoaderPhase(
    const std::filesystem::path& game_exe_path,
    std::uintptr_t module_base,
    int requested_mib) {
    if (requested_mib <= 0 || module_base == 0) {
        // Nothing to do. The outcome is still reported so the reader is
        // told the option was off rather than left to infer it.
        HeadroomPatchResult result{};
        result.requested_mib = requested_mib;
        result.loader_phase = true;
        result.outcome = requested_mib <= 0 ? HeadroomOutcome::OptionDisabled
                                            : HeadroomOutcome::NoGameModule;
        return result;
    }

    // The build identity has to be proven by the executable's SHA-256. The
    // researched RVAs mean nothing on another build, and a fixed-RVA write
    // is exactly the kind of write that needs that proof.
    std::array<unsigned char, 32> hash{};
    if (!spatch::ComputeSha256FromPath(game_exe_path, hash)) {
        HeadroomPatchResult result{};
        result.requested_mib = requested_mib;
        result.loader_phase = true;
        result.outcome = HeadroomOutcome::HashFailed;
        log::Warn(
            "pool_headroom requested but the game executable's SHA-256 "
            "could not be computed; headroom disabled");
        return result;
    }

    if (hash != spatch::build_info::kLegacySha256) {
        HeadroomPatchResult result{};
        result.requested_mib = requested_mib;
        result.loader_phase = true;
        result.hash_verified = true;
        result.unsupported_layout = true;
        result.outcome = HeadroomOutcome::UnsupportedLayout;
        log::Warn(
            "pool_headroom requested but the game build is not the "
            "researched layout; headroom disabled");
        return result;
    }

    return ApplyHeadroom(requested_mib, module_base, true, true, true);
}

bool WritePoolHeadroomReceipt(const std::filesystem::path& path,
                               const HeadroomPatchResult& result) {
    if (path.empty()) {
        return false;
    }

    std::ostringstream text;
    text << "SPatch Main Pool headroom\n"
         << "=========================\n\n"
         << "requested_mib=" << result.requested_mib << "\n"
         << "outcome=" << HeadroomOutcomeName(result.outcome) << "\n"
         << "applied=" << (result.applied ? 1 : 0) << "\n"
         << "loader_phase=" << (result.loader_phase ? 1 : 0) << "\n"
         << "hash_verified=" << (result.hash_verified ? 1 : 0) << "\n"
         << "pool_already_created="
         << (result.pool_already_created ? 1 : 0) << "\n"
         << "effective_reservation_mib=" << result.effective_mib << "\n"
         << "pool_creation_observed="
         << (result.pool_creation_observed ? 1 : 0) << "\n"
         << "effective_reservation_mib_after_creation="
         << result.effective_mib_after_creation << "\n"
         << "unsupported_layout=" << (result.unsupported_layout ? 1 : 0)
         << "\n"
         << "site_mismatch=" << (result.site_mismatch ? 1 : 0) << "\n\n";

    text << OutcomeProse(result) << "\n\n";

    if (result.effective_mib < 0) {
        text << "effective_reservation_mib is -1 because the pool did not\n"
                "exist yet when this was written, so there was no\n"
                "reservation to read back.\n\n";
    }

    if (result.pool_creation_observed) {
        text << "effective_reservation_mib_after_creation is the\n"
                "reservation the pool really has, read back from the pool\n"
                "object after the game created it.\n";
    } else {
        text << "effective_reservation_mib_after_creation is -1 because the\n"
                "pool's creation was not observed while this file was\n"
                "written. The crash-time pool report shows the reservation\n"
                "the pool has at the moment of a crash.\n";
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << text.str();
    stream.flush();
    return stream.good();
}

}  // namespace spatch::pool_headroom
