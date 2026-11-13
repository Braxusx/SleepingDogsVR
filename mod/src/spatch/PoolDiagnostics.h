#pragma once

#include <cstdint>
#include <string>

namespace spatch::pool_diagnostics {

// Layout of the engine's Main Pool object as created by the game at rva
// 0x426F70 in the legacy_researched layout (the layout the shipped
// sdhdship.exe sha256 C6DB199B... uses, and the layout the user's crash
// dumps came from). Every offset below was read out of the shipped
// executable's disassembly; nothing here is guessed.
//
// The pool object is a game global at rva 0x22581A0. It lives in the
// uninitialized tail of .data (vsize 0x4A7360 vs rawsize 0x107800), so it
// has no file bytes and the minidumps do not capture that range. The
// values below are therefore read from the live process, never from a dump.
//
// The magic at +0x10 makes the reader self-validating: a wrong RVA, a
// different build, or a pool that does not exist yet all read back as
// something other than 'CeV4', and the reader reports that instead of
// guessing.
struct PoolLayout {
    // The 'CeV4' magic the pool constructor writes at pool+0x10
    // (mov dword ptr [rcx+0x10], 0x34566543 at rva 0x17326E).
    std::uint32_t magic = 0x34566543;
    // Offset of the magic from the pool object base.
    std::uintptr_t magic_offset = 0x10;
    // Offset of the name-holder pointer. The constructor stores pool+0x28
    // here (mov qword ptr [rbx+0x20], rcx at rva 0x17327F, after
    // add rcx, 0x28 at rva 0x17327B).
    std::uintptr_t name_holder_offset = 0x20;
    // Offset of the name string pointer inside that holder. The holder's
    // first qword is the pool name, written by the name setup at rva
    // 0x18130E. The OOM report path reads the name the same way:
    // rdx = [rbp+0x20] then rdx = [rdx] (rvas 0x166CEA and 0x166CF5).
    std::uintptr_t name_pointer_offset = 0x00;
    // Offset of the segment base pointer. The constructor stores the
    // reservation base here (mov qword ptr [rbx+0x2430], r8 at rva
    // 0x173283).
    std::uintptr_t segment_base_offset = 0x2430;
    // Offset of the segment end pointer. The constructor stores base+size
    // here (lea rax, [r8+r9] then mov qword ptr [rbx+0x2438], rax at rvas
    // 0x17328A and 0x17328E), so the span between the two fields is the
    // size the pool reserved for itself. For the stock pool that span is
    // 500 MiB.
    std::uintptr_t segment_end_offset = 0x2438;
    // Offset of a state byte the pool keeps. The pool initializer clears
    // it (mov byte ptr [rcx+0x242c], 0 at rva 0x161739) and a separate
    // helper sets it (rva 0x16CF10). The allocator tests it at rva
    // 0x166B97 to choose between searching the pool's own space and
    // reserving a fresh region.
    std::uintptr_t state_flag_offset = 0x242C;
    // Offset of the parent-pool pointer. The allocator tests it at rva
    // 0x166C7E and, when it is set, escalates a request the pool cannot
    // serve to the pool that owns it. It is null for the stock Main Pool,
    // which is created with a null parent, so a request it cannot serve
    // is reported as an out of memory failure instead of being escalated.
    std::uintptr_t parent_pool_offset = 0x2440;
    // Offset of the fallback latch. The allocator sets it to 1 when the
    // pool could not serve a request itself and it fell back, to the
    // pool that owns this one or to a dedicated pool for that request
    // (mov dword ptr [rbp + 0x2448], 1 at rva 0x166C8F). It is never
    // cleared, so it is a latch rather than a counter.
    std::uintptr_t grow_count_offset = 0x2448;
    // RVA of the pool object itself.
    std::uintptr_t pool_rva = 0x22581A0;
};

// The state of the live Main Pool at the moment it was sampled.
struct PoolState {
    // The pool object was readable and carried the expected magic.
    bool valid = false;
    // The magic actually found (0 when the read failed).
    std::uint32_t magic = 0;
    // The pool's name, when the name pointer was readable.
    std::string name;
    // Segment base and end. The span between them is the size the pool
    // reserved for itself.
    std::uintptr_t segment_base = 0;
    std::uintptr_t segment_end = 0;
    // Segment span in bytes.
    std::uint64_t segment_bytes = 0;
    // The state byte and the growth counter described on the layout.
    bool state_flag = false;
    std::uint32_t grow_count = 0;
    // The parent-pool pointer, read raw.
    std::uint64_t parent_pool = 0;
};

// A history sample. Unlike PoolState this is plain data with no owning
// members, so a reader can copy it without a lock and without any risk of
// touching a half-destroyed object. The crash handler reads history while
// the sampler thread may be writing it, so the history must stay
// lock-free: a mutex there could deadlock the crash handler on the very
// thread that died holding it.
struct PoolSample {
    bool valid = false;
    std::uint32_t magic = 0;
    char name[64]{};
    std::uintptr_t segment_base = 0;
    std::uintptr_t segment_end = 0;
    std::uint64_t segment_bytes = 0;
    bool state_flag = false;
    std::uint32_t grow_count = 0;
    std::uint64_t parent_pool = 0;
};

// Where the crash happened, as far as the crash handler can tell.
//
// This is the decisive evidence about whether the Main Pool ran out of
// room. The pool's own fields cannot answer it: the pool does not keep a
// free-space total, and the growth counter only records that one path was
// taken. The faulting instruction can, because the Main Pool's allocator
// has exactly one place where it reports an out-of-memory failure and
// stops.
//
// A pool allocation that fails without stopping the game is reported by
// the game and the fault lands somewhere else, so a fault elsewhere is
// not proof that the pool had room. It is proof that the pool's own
// out-of-memory report is not what stopped the game.
struct CrashContext {
    // The faulting instruction's address, or 0 when it is unknown.
    std::uintptr_t fault_address = 0;
    // The Windows exception code, or 0 when it is unknown.
    //
    // The name avoids "exception_code" because the Windows SDK's
    // excpt.h defines that as a macro, which would rewrite the member
    // and every use of it.
    unsigned long fault_code = 0;
    // The rva of the faulting instruction in the game executable, or 0
    // when the address is not inside the game.
    std::uintptr_t fault_rva = 0;
    // The fault landed on the Main Pool's out-of-memory report trap.
    bool fault_at_out_of_memory_report = false;
};

// The rva of the Main Pool's out-of-memory report trap, in the
// legacy_researched layout.
//
// The allocator calls the game's report function and then executes an
// int3 at rva 0x166D0E. That int3 is what the user's crash dumps fault
// on, so a fault at this address means the game stopped because a
// request could not be served and the pool could not grow.
//
// This is the Main Pool allocator's only report-and-stop site, so it is
// a precise answer to "did the pool run out of room". The game has
// other out-of-memory reports, such as the fixed allocator's message at
// rva 0x17ACA3, but those report and return rather than stopping the
// process, so a fault cannot land on them.
inline constexpr std::uintptr_t kOutOfMemoryReportTrapRva = 0x00166D0E;

// The rva of the Main Pool's out-of-memory report format string, read by
// the report path at rva 0x166CFB.
inline constexpr std::uintptr_t kOutOfMemoryFormatRva = 0x0016ACBA0;

// Describes where the crash happened, relative to the game executable.
CrashContext DescribeCrash(std::uintptr_t game_module_base,
                            std::uintptr_t fault_address,
                            unsigned long fault_code);

// Reads the live pool object. Every read is SEH-guarded: a crash handler
// may run with a torn heap, and a faulting read must not fault again.
// Returns false when the pool object could not be read, or when the magic
// does not match (wrong build, or the pool does not exist yet).
bool ReadPoolState(std::uintptr_t game_module_base, PoolState* state);

// The same read, into plain data with no owning members. The crash handler
// uses this one, so it never has to allocate while the process is dying.
bool ReadPoolSample(std::uintptr_t game_module_base, PoolSample* sample);

// True when the pool object exists and carries the expected magic. This
// is how a caller tells "the pool has not been created yet" from "the pool
// is there and readable".
bool PoolExists(std::uintptr_t game_module_base);

// Waits for the game to create the pool and reports the reservation it
// ended up with, in MiB.
//
// The game creates the pool in the first moments after its entry point,
// so a caller that wants to know what the pool really got can arrive
// before that has happened. The wait is bounded by the budget, so a game
// that never creates the pool cannot hold the caller up.
//
// Returns -1 when the creation was not observed within the budget.
int WaitForPoolCreation(std::uintptr_t game_module_base,
                         unsigned int budget_milliseconds);

// Formats a pool state as one log line, without a trailing newline.
std::string FormatPoolState(const PoolState& state);

// The same, for a history sample.
std::string FormatPoolState(const PoolSample& sample);

// Formats a pool state as a multi-line report, for the crash-time file.
std::string FormatPoolReport(const PoolState& state,
                             const PoolSample* history,
                             std::size_t history_count,
                             const CrashContext& crash = CrashContext{});

// Formats the same report into a caller-provided buffer.
//
// This is the crash-path entry point. The caller owns the storage, so the
// crash handler never asks the allocator for memory: a crashing process
// may have a damaged heap, and a request for memory there can block on a
// lock the crashed thread was holding. Returns the number of characters
// written, excluding the terminator, and truncates if the buffer is too
// small.
std::size_t FormatPoolReportInto(char* buffer,
                                 std::size_t capacity,
                                 const PoolState& state,
                                 const PoolSample* history,
                                 std::size_t history_count,
                                 const CrashContext& crash = CrashContext{});

// The same, for a crash-time sample read as plain data.
std::size_t FormatPoolReportInto(char* buffer,
                                 std::size_t capacity,
                                 const PoolSample& state,
                                 const PoolSample* history,
                                 std::size_t history_count,
                                 const CrashContext& crash = CrashContext{});

// The layout the shipped executable uses.
const PoolLayout& Layout();

// The number of recent samples kept for the crash-time report.
inline constexpr std::size_t kPoolHistoryDepth = 8;

// Starts a background sampler that records the pool state on an interval
// so the crash-time report can show how the pool moved before it failed.
// The sampler only reads; it never writes to the game. Call StopSampler
// before the module is detached.
void StartSampler(std::uintptr_t game_module_base,
                 unsigned int interval_milliseconds);

// Asks the sampler to finish, without waiting for it.
//
// This is the safe call from a DLL_PROCESS_DETACH handler during process
// termination: by then the operating system has already ended the other
// threads, so waiting for one would hang on a thread that no longer runs.
void RequestSamplerStop();

// Asks the sampler to finish and gives up ownership of the thread.
//
// The process-termination call: the operating system has already ended
// the other threads, so waiting would hang, but the CRT still runs the
// sampler thread object's destructor after DllMain returns, and a
// destroyed thread that was never joined or detached terminates the
// process. Detaching keeps that destructor a no-op while the thread dies
// with the process.
void DetachSamplerForProcessExit();

// Asks the sampler to finish and waits for it. This must be called before
// the module's code is unmapped, because the sampler thread runs code
// inside this module.
void StopSampler();

// Copies the most recent samples into the caller's buffer, ordered oldest
// first. When the buffer is smaller than the history, the samples copied
// are the most recent ones that fit.
// Returns how many were copied. This never blocks: it is called from the
// crash handler, where waiting on a lock held by the crashed thread would
// hang the process instead of producing a report.
std::size_t CopyHistory(PoolSample* out, std::size_t capacity);

// Builds the name of the crash-time pool report from the name of the
// crash dump: the dump's ".dmp" suffix becomes ".pool.txt", so the two
// files sit together and are picked up together.
//
// The result is written into a caller-owned buffer and nothing is
// allocated. The crash handler runs on a thread that may have died
// holding the CRT heap lock, and a path built with a std::wstring would
// ask the allocator for memory and take that lock.
//
// Returns false when the name does not fit in the buffer. The report is a
// diagnostic, so a name that does not fit is skipped rather than
// truncated into a name that could point at the wrong file.
bool BuildPoolReportPath(const wchar_t* dump_path,
                         wchar_t* out,
                         std::size_t capacity);

}  // namespace spatch::pool_diagnostics
