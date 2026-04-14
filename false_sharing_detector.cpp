#include "pin.H"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <string>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define CACHE_LINE_SIZE 64
#define CACHE_LINE_MASK (~(ADDRINT)(CACHE_LINE_SIZE - 1))

/* =========================================================================
 * Data structures
 *
 * For each cache line we track which threads wrote to it, and exactly
 * which addresses each thread wrote to. We only need unique (thread, addr)
 * pairs — not every individual write — so we use sets. We do keep a total
 * write count per thread for heat-ranking the report.
 * ========================================================================= */

// All writes from one thread to one particular cache line
struct ThreadAccess {
    std::unordered_set<ADDRINT> addrs;  // unique addresses written by this thread
    ADDRINT  rep_ip;                    // first instruction pointer seen (for source info)
    UINT64   write_count;               // total writes (used to rank hotspots)

    ThreadAccess() : rep_ip(0), write_count(0) {}
};

// All writes to one cache line, across all threads
struct CacheLineRecord {
    std::unordered_map<THREADID, ThreadAccess> per_thread;
};

/* =========================================================================
 * Global state
 * ========================================================================= */

// Protects cache_line_records against concurrent access from multiple threads
static PIN_LOCK global_lock;

// Maps cache-line base address → record of all writes to that line
static std::unordered_map<ADDRINT, CacheLineRecord> cache_line_records;

/* =========================================================================
 * Analysis function — called at runtime for every memory write
 *
 * This is the hot path. It runs inside PIN's JIT-compiled code on every
 * write instruction the target program executes.
 * ========================================================================= */
VOID RecordWrite(THREADID tid, ADDRINT addr, ADDRINT ip)
{
    ADDRINT cl = addr & CACHE_LINE_MASK;  // cache line base address

    PIN_GetLock(&global_lock, tid + 1);

    CacheLineRecord& rec = cache_line_records[cl];
    ThreadAccess&    ta  = rec.per_thread[tid];

    ta.addrs.insert(addr);
    ta.write_count++;
    if (ta.rep_ip == 0) ta.rep_ip = ip;  // capture first IP for source lookup

    PIN_ReleaseLock(&global_lock);
}

/* =========================================================================
 * Instrumentation callback — called once per instruction during JIT
 *
 * PIN calls this for every instruction it compiles. We only instrument
 * instructions that belong to the main executable — this filters out
 * writes made by libc (memset, malloc, etc.), pthreads, and PIN's own
 * runtime, which would otherwise produce false positives.
 * ========================================================================= */
VOID Instruction(INS ins, VOID* v)
{
    // Only track writes from the target binary itself
    IMG img = IMG_FindByAddress(INS_Address(ins));
    if (!IMG_Valid(img) || !IMG_IsMainExecutable(img)) return;

    UINT32 memOps = INS_MemoryOperandCount(ins);

    for (UINT32 i = 0; i < memOps; i++) {
        if (INS_MemoryOperandIsWritten(ins, i)) {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE,
                (AFUNPTR)RecordWrite,
                IARG_THREAD_ID,
                IARG_MEMORYOP_EA, i,
                IARG_INST_PTR,
                IARG_END);
        }
    }
}

/* =========================================================================
 * Source location helper
 *
 * Attempts to resolve an instruction pointer to a source file and line.
 * Returns a human-readable string. Falls back gracefully if debug info
 * is unavailable.
 * ========================================================================= */
static std::string SourceLocation(ADDRINT ip)
{
    INT32       col  = 0;
    INT32       line = 0;
    std::string file;

    PIN_GetSourceLocation(ip, &col, &line, &file);

    if (file.empty()) return "(unknown source)";

    // Strip directory path — just show the filename for readability
    size_t slash = file.rfind('/');
    if (slash != std::string::npos) file = file.substr(slash + 1);

    return file + ":" + decstr(line);
}

/* =========================================================================
 * Fini — called once when the target program exits
 *
 * Scans all recorded cache lines and classifies each as:
 *   - No sharing      (only one thread wrote) → skip
 *   - True sharing    (multiple threads, all to the exact same address) → info
 *   - False sharing   (multiple threads, different addresses) → hotspot report
 *
 * Hotspots are sorted by total write count so the hottest appear first.
 * ========================================================================= */
VOID Fini(INT32 code, VOID* v)
{
    // -----------------------------------------------------------------------
    // Separate cache lines into false-sharing hotspots and true-sharing notes
    // -----------------------------------------------------------------------

    using ActiveList = std::vector<std::pair<THREADID, ThreadAccess*>>;
    struct Hotspot {
        ADDRINT    cl;           // cache line base address
        UINT64     total_writes; // sum across active threads
        int        num_threads;  // number of active writing threads
        ActiveList active;       // filtered thread list (no init-only threads)
    };

    std::vector<Hotspot> false_sharing;
    std::vector<Hotspot> true_sharing;

    for (auto& kv : cache_line_records) {
        ADDRINT              cl  = kv.first;
        CacheLineRecord&     rec = kv.second;

        // Find the maximum write count for any single thread on this cache line.
        // We use this to filter out threads with very few writes (< 0.1% of max)
        // which are initialization stores (e.g., a single memset-derived store in
        // main() that happens to touch the same cache line as the benchmark data).
        UINT64 max_thread_writes = 0;
        for (auto& tv : rec.per_thread)
            if (tv.second.write_count > max_thread_writes)
                max_thread_writes = tv.second.write_count;

        // Build a filtered view: only threads that are "active" writers
        std::vector<std::pair<THREADID, ThreadAccess*>> active;
        for (auto& tv : rec.per_thread) {
            if (tv.second.write_count * 1000 >= max_thread_writes)  // >= 0.1% of max
                active.push_back({tv.first, &tv.second});
        }

        int num_threads = (int)active.size();
        if (num_threads < 2) continue;  // single thread — no sharing at all

        // Collect every unique address written across all active threads
        std::unordered_set<ADDRINT> all_addrs;
        UINT64 total_writes = 0;
        for (auto& av : active) {
            for (ADDRINT a : av.second->addrs) all_addrs.insert(a);
            total_writes += av.second->write_count;
        }

        if (all_addrs.size() == 1) {
            // Every thread wrote the exact same address → true sharing / data race
            true_sharing.push_back({cl, total_writes, num_threads, active});
        } else {
            // Threads wrote different addresses on the same cache line → false sharing
            false_sharing.push_back({cl, total_writes, num_threads, active});
        }
    }

    // Sort hotspots hottest first
    std::sort(false_sharing.begin(), false_sharing.end(),
              [](const Hotspot& a, const Hotspot& b) {
                  return a.total_writes > b.total_writes;
              });

    // -----------------------------------------------------------------------
    // Print false sharing report
    // -----------------------------------------------------------------------

    std::cerr << "\n";
    std::cerr << "=================================================================\n";
    std::cerr << "  False Sharing Detector Report\n";
    std::cerr << "=================================================================\n";

    if (false_sharing.empty()) {
        std::cerr << "\nNo false sharing detected.\n";
    } else {
        for (auto& h : false_sharing) {
            // Severity: HIGH for 3+ writing threads, MEDIUM for 2
            const char* severity = (h.num_threads >= 3) ? "HIGH  " : "MEDIUM";

            std::cerr << "\n[" << severity << "]  "
                      << "Cache line 0x" << std::hex << h.cl << std::dec
                      << "  |  " << h.num_threads << " writing threads"
                      << "  |  " << h.total_writes << " total writes\n";

            // Print each thread's contribution
            for (auto& av : h.active) {
                THREADID            tid = av.first;
                const ThreadAccess& ta  = *av.second;

                for (ADDRINT a : ta.addrs) {
                    std::cerr << "  Thread " << tid
                              << "  ->  0x" << std::hex << a << std::dec
                              << "  (" << SourceLocation(ta.rep_ip) << ")"
                              << "  [" << ta.write_count << " writes]\n";
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Print true sharing notes (data races, not false sharing)
    // -----------------------------------------------------------------------

    if (!true_sharing.empty()) {
        std::cerr << "\n-----------------------------------------------------------------\n";
        std::cerr << "  True Sharing (data races — NOT false sharing)\n";
        std::cerr << "-----------------------------------------------------------------\n";

        for (auto& h : true_sharing) {
            ADDRINT shared_addr = *h.active.begin()->second->addrs.begin();
            std::cerr << "\n[INFO]   Cache line 0x" << std::hex << h.cl << std::dec
                      << "  |  " << h.num_threads << " threads"
                      << "  |  all writing 0x" << std::hex << shared_addr << std::dec
                      << "  |  " << h.total_writes << " total writes\n";
            std::cerr << "         -> This is a data race on a single variable,"
                         " not false sharing.\n";
        }
    }

    std::cerr << "\n=================================================================\n\n";
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char* argv[])
{
    PIN_InitLock(&global_lock);

    // Must be called before PIN_Init() for source location lookup to work
    PIN_InitSymbols();

    if (PIN_Init(argc, argv)) {
        std::cerr << "PIN_Init failed\n";
        return 1;
    }

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_StartProgram();  // does not return
    return 0;
}
