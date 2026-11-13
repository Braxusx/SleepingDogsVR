#include "PoolDiagnostics.h"

#include "Logger.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace spatch::pool_diagnostics {
namespace {

const PoolLayout& LayoutImpl() {
    static const PoolLayout layout{};
    return layout;
}

// Every read below is SEH-guarded. The pool object lives in the game's
// uninitialized .data tail: it is a valid mapped address whenever the game
// is alive, but a crash handler may run while the game is tearing down, and
// a faulting read inside the crash filter would fault again.
bool SafeReadUint32(std::uintptr_t address, std::uint32_t* value) {
    __try {
        *value = *reinterpret_cast<const std::uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadUint64(std::uintptr_t address, std::uint64_t* value) {
    __try {
        *value = *reinterpret_cast<const std::uint64_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadByte(std::uintptr_t address, std::uint8_t* value) {
    __try {
        *value = *reinterpret_cast<const std::uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies a NUL-terminated string out of game memory. The pool name is a
// literal in the shipped image, so it is always readable when the pool
// object is; the guard is there so a torn pointer cannot fault the reader.
bool SafeReadCString(std::uintptr_t address,
                     char* buffer,
                     std::size_t capacity) {
    if (capacity == 0) {
        return false;
    }
    __try {
        std::size_t index = 0;
        for (; index + 1 < capacity; ++index) {
            const char character =
                *reinterpret_cast<const char*>(address + index);
            if (character == '\0') {
                break;
            }
            buffer[index] = character;
        }
        buffer[index] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Recent samples, kept so the crash-time report can show how the pool
// moved before it failed.
//
// The writer is the sampler thread and the reader is the crash handler.
// The crash handler must never wait on a lock, because the thread that
// died may have been holding it, so the ring is lock-free. A slot is
// published with a release store of its sequence number after the sample
// is written; a reader only trusts a slot whose sequence number is even
// and unchanged across its copy, and otherwise falls back to the previous
// good copy. A torn sample would report a slightly wrong number in a
// diagnostic file, which is a far better outcome than a hung crash
// handler.
struct HistorySlot {
    std::atomic<std::uint64_t> sequence{0};
    PoolSample sample{};
};

HistorySlot g_history[kPoolHistoryDepth];
std::atomic<std::uint32_t> g_history_count{0};
std::atomic<std::uint32_t> g_history_next{0};

std::atomic<bool> g_sampler_running{false};
std::thread g_sampler_thread;
// The sampler waits on this event rather than sleeping in slices, so it
// wakes once per interval instead of ten times, and it stops as soon as
// the event is set.
HANDLE g_sampler_wake = nullptr;

}  // namespace

const PoolLayout& Layout() {
    return LayoutImpl();
}

bool ReadPoolSample(std::uintptr_t game_module_base, PoolSample* sample) {
    if (sample == nullptr || game_module_base == 0) {
        return false;
    }
    *sample = PoolSample{};

    const PoolLayout& layout = LayoutImpl();
    const std::uintptr_t pool = game_module_base + layout.pool_rva;

    std::uint32_t magic = 0;
    if (!SafeReadUint32(pool + layout.magic_offset, &magic)) {
        return false;
    }
    sample->magic = magic;
    if (magic != layout.magic) {
        // The pool has not been created yet, or this is not the pool we
        // mapped. Either way there is nothing meaningful to report.
        return false;
    }

    sample->valid = true;

    std::uint8_t state_flag = 0;
    if (SafeReadByte(pool + layout.state_flag_offset, &state_flag)) {
        sample->state_flag = state_flag != 0;
    }

    std::uint64_t base = 0;
    std::uint64_t end = 0;
    if (SafeReadUint64(pool + layout.segment_base_offset, &base) &&
        SafeReadUint64(pool + layout.segment_end_offset, &end)) {
        sample->segment_base = static_cast<std::uintptr_t>(base);
        sample->segment_end = static_cast<std::uintptr_t>(end);
        sample->segment_bytes = end > base ? end - base : 0;
    }

    std::uint32_t grow_count = 0;
    if (SafeReadUint32(pool + layout.grow_count_offset, &grow_count)) {
        sample->grow_count = grow_count;
    }

    std::uint64_t parent_pool = 0;
    if (SafeReadUint64(pool + layout.parent_pool_offset, &parent_pool)) {
        sample->parent_pool = parent_pool;
    }

    // The name pointer is reached through the holder: pool+0x20 points at
    // the holder, and the holder's first qword is the name string pointer.
    std::uint64_t holder = 0;
    if (SafeReadUint64(pool + layout.name_holder_offset, &holder) &&
        holder != 0) {
        std::uint64_t name_pointer = 0;
        if (SafeReadUint64(static_cast<std::uintptr_t>(holder) +
                               layout.name_pointer_offset,
                           &name_pointer) &&
            name_pointer != 0) {
            SafeReadCString(static_cast<std::uintptr_t>(name_pointer),
                            sample->name,
                            sizeof(sample->name));
        }
    }

    return true;
}

bool ReadPoolState(std::uintptr_t game_module_base, PoolState* state) {
    if (state == nullptr) {
        return false;
    }
    *state = PoolState{};

    PoolSample sample{};
    if (!ReadPoolSample(game_module_base, &sample)) {
        return false;
    }

    state->valid = sample.valid;
    state->magic = sample.magic;
    state->name.assign(sample.name);
    state->segment_base = sample.segment_base;
    state->segment_end = sample.segment_end;
    state->segment_bytes = sample.segment_bytes;
    state->state_flag = sample.state_flag;
    state->grow_count = sample.grow_count;
    state->parent_pool = sample.parent_pool;
    return true;
}

bool PoolExists(std::uintptr_t game_module_base) {
    PoolState state;
    return ReadPoolState(game_module_base, &state);
}

int WaitForPoolCreation(std::uintptr_t game_module_base,
                         unsigned int budget_milliseconds) {
    if (game_module_base == 0) {
        return -1;
    }

    // The pool's constructor writes its magic before it records the
    // segment it reserved, so a pool that is being created reads back
    // without a segment for a moment. The reservation is only reported
    // once the segment is there, otherwise a half-built pool would report
    // a reservation of nothing.
    const ULONGLONG deadline = GetTickCount64() + budget_milliseconds;
    for (;;) {
        PoolState state;
        if (ReadPoolState(game_module_base, &state) && state.segment_bytes != 0) {
            return static_cast<int>(state.segment_bytes / (1024ull * 1024ull));
        }
        if (GetTickCount64() >= deadline) {
            return -1;
        }
        Sleep(1);
    }
}

std::string FormatPoolState(const PoolState& state) {
    char line[512]{};
    const int written = std::snprintf(
        line,
        sizeof(line),
        "pool_state valid=%d magic=0x%08X name=\"%s\" "
        "segment=0x%llX-0x%llX segment_bytes=%llu state_flag=%d "
        "grow_count=%u parent_pool=0x%llX",
        state.valid ? 1 : 0,
        state.magic,
        state.name.c_str(),
        static_cast<unsigned long long>(state.segment_base),
        static_cast<unsigned long long>(state.segment_end),
        static_cast<unsigned long long>(state.segment_bytes),
        state.state_flag ? 1 : 0,
        state.grow_count,
        static_cast<unsigned long long>(state.parent_pool));
    if (written <= 0) {
        return "pool_state valid=0 read_failed=1";
    }
    return std::string(line, static_cast<std::size_t>(written));
}

std::string FormatPoolState(const PoolSample& sample) {
    char line[512]{};
    const int written = std::snprintf(
        line,
        sizeof(line),
        "pool_state valid=%d magic=0x%08X name=\"%s\" "
        "segment=0x%llX-0x%llX segment_bytes=%llu state_flag=%d "
        "grow_count=%u parent_pool=0x%llX",
        sample.valid ? 1 : 0,
        sample.magic,
        sample.name,
        static_cast<unsigned long long>(sample.segment_base),
        static_cast<unsigned long long>(sample.segment_end),
        static_cast<unsigned long long>(sample.segment_bytes),
        sample.state_flag ? 1 : 0,
        sample.grow_count,
        static_cast<unsigned long long>(sample.parent_pool));
    if (written <= 0) {
        return "pool_state valid=0 read_failed=1";
    }
    return std::string(line, static_cast<std::size_t>(written));
}

namespace {

// The same formatting, into a caller-owned buffer, so the crash path does
// not have to allocate.
void FormatPoolStateInto(char* buffer,
                         std::size_t capacity,
                         const PoolState& state) {
    if (buffer == nullptr || capacity == 0) {
        return;
    }
    const int written = std::snprintf(
        buffer,
        capacity,
        "pool_state valid=%d magic=0x%08X name=\"%s\" "
        "segment=0x%llX-0x%llX segment_bytes=%llu state_flag=%d "
        "grow_count=%u parent_pool=0x%llX",
        state.valid ? 1 : 0,
        state.magic,
        state.name.c_str(),
        static_cast<unsigned long long>(state.segment_base),
        static_cast<unsigned long long>(state.segment_end),
        static_cast<unsigned long long>(state.segment_bytes),
        state.state_flag ? 1 : 0,
        state.grow_count,
        static_cast<unsigned long long>(state.parent_pool));
    if (written <= 0) {
        std::snprintf(buffer, capacity, "pool_state valid=0 read_failed=1");
    }
}

void FormatPoolStateInto(char* buffer,
                         std::size_t capacity,
                         const PoolSample& sample) {
    if (buffer == nullptr || capacity == 0) {
        return;
    }
    const int written = std::snprintf(
        buffer,
        capacity,
        "pool_state valid=%d magic=0x%08X name=\"%s\" "
        "segment=0x%llX-0x%llX segment_bytes=%llu state_flag=%d "
        "grow_count=%u parent_pool=0x%llX",
        sample.valid ? 1 : 0,
        sample.magic,
        sample.name,
        static_cast<unsigned long long>(sample.segment_base),
        static_cast<unsigned long long>(sample.segment_end),
        static_cast<unsigned long long>(sample.segment_bytes),
        sample.state_flag ? 1 : 0,
        sample.grow_count,
        static_cast<unsigned long long>(sample.parent_pool));
    if (written <= 0) {
        std::snprintf(buffer, capacity, "pool_state valid=0 read_failed=1");
    }
}

}  // namespace

namespace {

// Appends to a fixed buffer without allocating. Returns false when the
// text did not fit; the buffer is always left terminated.
bool AppendText(char* buffer,
                std::size_t capacity,
                std::size_t& used,
                const char* text) {
    if (buffer == nullptr || capacity == 0) {
        return false;
    }
    const std::size_t length = std::strlen(text);
    if (used + length + 1 > capacity) {
        buffer[used] = '\0';
        return false;
    }
    std::memcpy(buffer + used, text, length);
    used += length;
    buffer[used] = '\0';
    return true;
}

}  // namespace

namespace {

// The part of the report that explains the fields. Shared by both report
// builders so the wording cannot drift apart.
const char* const kPoolFieldMeanings =
    "Field meanings, all read from the shipped executable:\n"
    "  name           the pool's name; the stock pool is \"Main Pool\"\n"
    "  segment_bytes  size the pool reserved for itself; the stock\n"
    "                 reservation is 500 MiB (0x1F400000)\n"
    "  state_flag     byte the pool keeps; the allocator tests it to\n"
    "                 choose between the pool's own space and a fresh\n"
    "                 region\n"
    "  grow_count     latch, set to 1 when the pool could not serve a\n"
    "                 request itself and the allocator fell back\n"
    "  parent_pool    pointer to the pool that owns this one; null for\n"
    "                 the stock Main Pool\n"
    "\n"
    "How to read this:\n"
    "  The pool does not keep a free-space total, so these fields cannot\n"
    "  say how full it was. The decisive answer is the fault address\n"
    "  above: the Main Pool's allocator has exactly one place where it\n"
    "  reports an out of memory failure and stops, and a fault there\n"
    "  means the pool could not serve the request.\n"
    "  grow_count is not a counter. It is a latch, and it is never\n"
    "  cleared. It is set when the pool could not serve a request\n"
    "  itself, and the allocator fell back: to the pool that owns this\n"
    "  one, or to a dedicated pool for that request. Neither of those\n"
    "  enlarges this pool's own reservation. A grow_count of 0 means\n"
    "  the allocator never fell back for this pool. It says nothing\n"
    "  about free space.\n"
    "  A pool allocation that fails without stopping the game is\n"
    "  reported by the game and the fault lands somewhere else. The\n"
    "  grow_count latch is the only trace of that in this report.\n"
    "  segment_bytes is the reservation the pool was given, not how much\n"
    "  of it was in use.";

void AppendHistory(char* buffer,
                   std::size_t capacity,
                   std::size_t& used,
                   const PoolSample* history,
                   std::size_t history_count) {
    if (history == nullptr || history_count == 0) {
        return;
    }
    AppendText(buffer, capacity, used, "earlier samples (oldest first):\n");
    for (std::size_t index = 0; index < history_count; ++index) {
        char line[512]{};
        FormatPoolStateInto(line, sizeof(line), history[index]);
        AppendText(buffer, capacity, used, "  ");
        AppendText(buffer, capacity, used, line);
        AppendText(buffer, capacity, used, "\n");
    }
    AppendText(buffer, capacity, used, "\n");
}

// Writes the crash location and the verdict it supports.
void AppendCrashContext(char* buffer,
                        std::size_t capacity,
                        std::size_t& used,
                        const CrashContext& crash) {
    if (crash.fault_address == 0) {
        AppendText(buffer,
                   capacity,
                   used,
                   "crash location: not available\n");
        return;
    }

    char line[512]{};
    std::snprintf(line,
                  sizeof(line),
                  "crash location: exception_code=0x%08lX fault_rva=0x%llX\n",
                  static_cast<unsigned long>(crash.fault_code),
                  static_cast<unsigned long long>(crash.fault_rva));
    AppendText(buffer, capacity, used, line);

    if (crash.fault_at_out_of_memory_report) {
        AppendText(
            buffer,
            capacity,
            used,
            "verdict: the game stopped on its own out of memory report, so\n"
            "         the pool could not serve the request. This is the\n"
            "         game reporting a failed allocation, not a fault in\n"
            "         SPatch.\n");
    } else {
        AppendText(
            buffer,
            capacity,
            used,
            "verdict: the fault is not the Main Pool's out of memory report,\n"
            "         so the pool running out of room is not what stopped\n"
            "         the game here. A pool allocation that failed without\n"
            "         stopping the game would be reported by the game and\n"
            "         the fault would land somewhere else; the grow_count\n"
            "         latch below is the only trace of that.\n");
    }
}

}  // namespace

std::size_t FormatPoolReportInto(char* buffer,
                                 std::size_t capacity,
                                 const PoolState& state,
                                 const PoolSample* history,
                                 std::size_t history_count,
                                 const CrashContext& crash) {
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }
    buffer[0] = '\0';
    std::size_t used = 0;

    AppendText(buffer, capacity, used, "SPatch Main Pool state\n");
    AppendText(buffer, capacity, used, "=====================\n\n");
    AppendCrashContext(buffer, capacity, used, crash);
    AppendText(buffer, capacity, used, "\n");
    AppendText(buffer, capacity, used, "crash-time sample:\n  ");
    {
        char line[512]{};
        FormatPoolStateInto(line, sizeof(line), state);
        AppendText(buffer, capacity, used, line);
    }
    AppendText(buffer, capacity, used, "\n\n");
    AppendHistory(buffer, capacity, used, history, history_count);
    AppendText(buffer, capacity, used, kPoolFieldMeanings);
    return used;
}

std::size_t FormatPoolReportInto(char* buffer,
                                 std::size_t capacity,
                                 const PoolSample& state,
                                 const PoolSample* history,
                                 std::size_t history_count,
                                 const CrashContext& crash) {
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }
    buffer[0] = '\0';
    std::size_t used = 0;

    AppendText(buffer, capacity, used, "SPatch Main Pool state\n");
    AppendText(buffer, capacity, used, "=====================\n\n");
    AppendCrashContext(buffer, capacity, used, crash);
    AppendText(buffer, capacity, used, "\n");
    AppendText(buffer, capacity, used, "crash-time sample:\n  ");
    {
        char line[512]{};
        FormatPoolStateInto(line, sizeof(line), state);
        AppendText(buffer, capacity, used, line);
    }
    AppendText(buffer, capacity, used, "\n\n");
    AppendHistory(buffer, capacity, used, history, history_count);
    AppendText(buffer, capacity, used, kPoolFieldMeanings);
    return used;
}

std::string FormatPoolReport(const PoolState& state,
                             const PoolSample* history,
                             std::size_t history_count,
                             const CrashContext& crash) {
    // Large enough for the report and every sample line it can carry.
    std::array<char, 8192> buffer{};
    const std::size_t used = FormatPoolReportInto(
        buffer.data(), buffer.size(), state, history, history_count, crash);
    return std::string(buffer.data(), used);
}

CrashContext DescribeCrash(std::uintptr_t game_module_base,
                            std::uintptr_t fault_address,
                            unsigned long fault_code) {
    CrashContext crash{};
    crash.fault_address = fault_address;
    crash.fault_code = fault_code;
    if (game_module_base == 0 || fault_address < game_module_base) {
        return crash;
    }
    crash.fault_rva = fault_address - game_module_base;
    crash.fault_at_out_of_memory_report =
        crash.fault_rva == kOutOfMemoryReportTrapRva;
    return crash;
}

std::size_t CopyHistory(PoolSample* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return 0;
    }

    // The ring's logical size and head are read once. A sample published
    // while this runs is simply not part of this snapshot, which is fine:
    // the report is a picture of the pool, not a transaction.
    const std::uint32_t count = g_history_count.load(std::memory_order_acquire);
    const std::uint32_t next = g_history_next.load(std::memory_order_acquire);
    const std::size_t wanted =
        capacity < count ? capacity : static_cast<std::size_t>(count);

    std::size_t copied = 0;
    for (std::size_t index = 0; index < wanted; ++index) {
        const std::size_t slot =
            (next + kPoolHistoryDepth - wanted + index) % kPoolHistoryDepth;
        const std::uint64_t before =
            g_history[slot].sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            // The slot is mid-write. Skip it rather than report a torn
            // sample; a missing sample is harmless in a diagnostic file.
            continue;
        }
        const PoolSample sample = g_history[slot].sample;
        const std::uint64_t after =
            g_history[slot].sequence.load(std::memory_order_acquire);
        if (before != after) {
            // The sample changed under the copy. Skip it for the same
            // reason.
            continue;
        }
        out[copied++] = sample;
    }
    return copied;
}

bool BuildPoolReportPath(const wchar_t* dump_path,
                         wchar_t* out,
                         std::size_t capacity) {
    if (dump_path == nullptr || out == nullptr || capacity == 0) {
        return false;
    }

    out[0] = L'\0';

    std::size_t length = 0;
    while (length + 1 < capacity && dump_path[length] != L'\0') {
        out[length] = dump_path[length];
        ++length;
    }
    if (dump_path[length] != L'\0') {
        // The name did not fit, so it was truncated. The truncated name
        // would point at a different file, so the report is skipped.
        out[0] = L'\0';
        return false;
    }
    out[length] = L'\0';

    // Replace the dump's ".dmp" suffix with the report's ".pool.txt".
    constexpr wchar_t kDumpSuffix[] = L".dmp";
    constexpr wchar_t kReportSuffix[] = L".pool.txt";
    constexpr std::size_t kDumpSuffixLength = 4;
    constexpr std::size_t kReportSuffixLength = 9;

    if (length >= kDumpSuffixLength) {
        const wchar_t* tail = out + length - kDumpSuffixLength;
        bool matches = true;
        for (std::size_t index = 0; index < kDumpSuffixLength; ++index) {
            if (tail[index] != kDumpSuffix[index]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            length -= kDumpSuffixLength;
        }
    }

    if (length + kReportSuffixLength + 1 > capacity) {
        out[0] = L'\0';
        return false;
    }
    for (std::size_t index = 0; index < kReportSuffixLength; ++index) {
        out[length + index] = kReportSuffix[index];
    }
    out[length + kReportSuffixLength] = L'\0';
    return true;
}

void StartSampler(std::uintptr_t game_module_base,
                 unsigned int interval_milliseconds) {
    if (game_module_base == 0 || interval_milliseconds == 0) {
        return;
    }
    if (g_sampler_running.exchange(true)) {
        return;
    }

    g_sampler_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_sampler_wake == nullptr) {
        g_sampler_running.store(false, std::memory_order_release);
        return;
    }
    ResetEvent(g_sampler_wake);

    g_sampler_thread = std::thread([game_module_base, interval_milliseconds]() {
        while (g_sampler_running.load()) {
            PoolState state;
            if (ReadPoolState(game_module_base, &state)) {
                const std::uint32_t slot =
                    g_history_next.load(std::memory_order_relaxed) %
                    kPoolHistoryDepth;

                PoolSample sample{};
                sample.valid = state.valid;
                sample.magic = state.magic;
                std::memcpy(sample.name,
                            state.name.c_str(),
                            state.name.size() < sizeof(sample.name) - 1
                                ? state.name.size()
                                : sizeof(sample.name) - 1);
                sample.segment_base = state.segment_base;
                sample.segment_end = state.segment_end;
                sample.segment_bytes = state.segment_bytes;
                sample.state_flag = state.state_flag;
                sample.grow_count = state.grow_count;
                sample.parent_pool = state.parent_pool;

                // Mark the slot as being written, publish the sample, then
                // mark it as settled. A reader that sees an odd sequence
                // skips the slot.
                const std::uint64_t sequence =
                    g_history[slot].sequence.load(std::memory_order_relaxed);
                g_history[slot].sequence.store(sequence + 1,
                                               std::memory_order_release);
                std::atomic_thread_fence(std::memory_order_release);
                g_history[slot].sample = sample;
                g_history[slot].sequence.store(sequence + 2,
                                               std::memory_order_release);

                const std::uint32_t written =
                    g_history_next.load(std::memory_order_relaxed);
                g_history_next.store((written + 1) % kPoolHistoryDepth,
                                     std::memory_order_release);
                std::uint32_t published =
                    g_history_count.load(std::memory_order_relaxed);
                if (published < kPoolHistoryDepth) {
                    g_history_count.store(published + 1,
                                          std::memory_order_release);
                }
            }

            // One wait for the whole interval. The event is set when the
            // sampler is asked to stop, so the thread leaves at once
            // instead of after the remaining interval.
            WaitForSingleObject(g_sampler_wake, interval_milliseconds);
        }
    });
}

void RequestSamplerStop() {
    g_sampler_running.store(false, std::memory_order_release);
    if (g_sampler_wake != nullptr) {
        SetEvent(g_sampler_wake);
    }
}

void StopSampler() {
    RequestSamplerStop();
    if (g_sampler_thread.joinable()) {
        g_sampler_thread.join();
    }
    if (g_sampler_wake != nullptr) {
        CloseHandle(g_sampler_wake);
        g_sampler_wake = nullptr;
    }
}

void DetachSamplerForProcessExit() {
    RequestSamplerStop();
    if (g_sampler_thread.joinable()) {
        g_sampler_thread.detach();
    }
}

}  // namespace spatch::pool_diagnostics
