#include "pin.H"
#include <iostream>
#include <unordered_map>
#include <map>
#include <vector>
#include <algorithm>
#include <string>

#define CACHE_LINE_SIZE 64
#define CACHE_LINE_MASK (~(ADDRINT)(CACHE_LINE_SIZE - 1))
#define MAX_THREADS 64

#define MAX_TRACKED_IPS 8

struct CacheLineRecord {
    uint64_t reads;
    uint64_t writes;
    uint64_t addr_mask;
    ADDRINT  write_ips[MAX_TRACKED_IPS];
    ADDRINT  read_ips[MAX_TRACKED_IPS];
    uint8_t  write_ip_count;
    uint8_t  read_ip_count;
};

static std::unordered_map<ADDRINT, CacheLineRecord> instrumentation_records[MAX_THREADS];

// get data on a given memory access and store it in the larger datastructure
VOID RecordAccess(VOID* addr, BOOL is_write, ADDRINT ip, THREADID tid) {
    ADDRINT mem_addr          = (ADDRINT)addr;
    ADDRINT cache_line = mem_addr & CACHE_LINE_MASK;
    CacheLineRecord& rec = instrumentation_records[tid].try_emplace(cache_line).first->second;

    rec.reads  += is_write ? 0 : 1;
    rec.writes += is_write ? 1 : 0;
    rec.addr_mask |= (1ULL << (mem_addr & 63));

    if (is_write) {
        bool found = false;
        for (uint8_t i = 0; i < rec.write_ip_count; i++)
            if (rec.write_ips[i] == ip) { found = true; break; }
        if (!found && rec.write_ip_count < MAX_TRACKED_IPS)
            rec.write_ips[rec.write_ip_count++] = ip;
    } else {
        bool found = false;
        for (uint8_t i = 0; i < rec.read_ip_count; i++)
            if (rec.read_ips[i] == ip) { found = true; break; }
        if (!found && rec.read_ip_count < MAX_TRACKED_IPS)
            rec.read_ips[rec.read_ip_count++] = ip;
    }
}

VOID Instruction(INS ins, VOID* v) {
    if (INS_IsMemoryWrite(ins)) {
        INS_InsertPredicatedCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)RecordAccess,
            IARG_MEMORYWRITE_EA,
            IARG_BOOL, TRUE,
            IARG_INST_PTR,
            IARG_THREAD_ID,
            IARG_END
        );
    }

    if (INS_IsMemoryRead(ins)) {
        INS_InsertPredicatedCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)RecordAccess,
            IARG_MEMORYREAD_EA,
            IARG_BOOL, FALSE,
            IARG_INST_PTR,
            IARG_THREAD_ID,
            IARG_END
        );
    }

    if (INS_HasMemoryRead2(ins)) {
        INS_InsertPredicatedCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)RecordAccess,
            IARG_MEMORYREAD2_EA,
            IARG_BOOL, FALSE,
            IARG_INST_PTR,
            IARG_THREAD_ID,
            IARG_END
        );
    }
}


// NEEDS HUMAN REVIEW STILL
// ------------------------
VOID Fini(INT32 code, VOID* v) {
    // =========================================================================
    // Step 1: invert [tid][cache_line] -> [cache_line][tid]
    // and compute total writes across all threads for threshold calculation
    // =========================================================================
    // std::map (tree-based) avoids the GCC/libc++ false positive that
    // unordered_map triggers at -O3; Fini runs once so O(log n) is fine
    std::map<ADDRINT, std::map<int, const CacheLineRecord*>> by_line;
    uint64_t total_writes = 0;

    for (int t = 0; t < MAX_THREADS; t++) {
        for (auto& [cache_line, rec] : instrumentation_records[t]) {
            by_line[cache_line][t] = &rec;
            total_writes += rec.writes;
        }
    }

    // =========================================================================
    // Step 2: collect hotspots
    //
    // A cache line is a hotspot if:
    //   1. more than one thread accessed it
    //   2. writes on this line exceed 0.1% of all writes (filters init-only stores)
    //   3. threads touched different byte offsets (false sharing) OR
    //      the same offset (true sharing — report both, label differently)
    // =========================================================================
    struct Hotspot {
        ADDRINT  cache_line;
        uint64_t line_writes;
        bool     is_false_sharing;
        std::map<int, const CacheLineRecord*> threads;
    };

    std::vector<Hotspot> hotspots;
    uint64_t threshold = total_writes / 1000; // 0.1%

    for (auto& [cache_line, thread_map] : by_line) {
        if (thread_map.size() < 2) continue;

        uint64_t line_writes = 0;
        for (auto& [tid, rec] : thread_map)
            line_writes += rec->writes;
        if (line_writes <= threshold) continue;

        // OR all per-thread addr_masks together — each set bit is a byte offset
        // touched by at least one thread on this cache line.
        // popcount > 1 means threads accessed different addresses -> false sharing.
        // popcount == 1 means all threads touched the same byte offset -> true sharing.
        uint64_t combined_mask = 0;
        for (auto& [tid, rec] : thread_map)
            combined_mask |= rec->addr_mask;
        bool is_false_sharing = __builtin_popcountll(combined_mask) > 1;

        hotspots.push_back({cache_line, line_writes, is_false_sharing, thread_map});
    }

    // =========================================================================
    // Step 3: rank hotspots by write count, most writes first
    // =========================================================================
    std::sort(hotspots.begin(), hotspots.end(),
        [](const Hotspot& a, const Hotspot& b) {
            return a.line_writes > b.line_writes;
        });

    // =========================================================================
    // Step 4: report
    // =========================================================================
    std::cerr << "\n[detector] false sharing report\n";
    std::cerr << "=========================================\n";

    int rank = 0;
    for (auto& hs : hotspots) {
        rank++;
        std::cerr << "\nHotspot " << rank
                  << " [" << (hs.is_false_sharing ? "FALSE SHARING" : "TRUE SHARING") << "]"
                  << ": cache line 0x" << std::hex << hs.cache_line << std::dec << "\n";
        std::cerr << "  total writes: " << hs.line_writes << "\n";
        std::cerr << "  threads: " << hs.threads.size() << "\n";

        for (auto& [tid, rec] : hs.threads) {
            std::cerr << "    thread " << tid
                      << ": " << rec->writes << " writes, "
                      << rec->reads  << " reads\n";

            for (ADDRINT ip : rec->write_ips) {
                std::string file;
                INT32 line = 0, col = 0;
                PIN_GetSourceLocation(ip, &col, &line, &file);
                if (!file.empty())
                    std::cerr << "      write at " << file << ":" << line << "\n";
            }
        }
    }

    if (rank == 0)
        std::cerr << "no hotspots detected above threshold\n";

    std::cerr << "=========================================\n";
    std::cerr << "[detector] done. total writes observed: " << total_writes << "\n";
}

int main(int argc, char* argv[]) {
    PIN_InitSymbols();  // must come before PIN_Init for source location lookup to work
    PIN_Init(argc, argv);
    INS_AddInstrumentFunction(Instruction, nullptr);
    PIN_AddFiniFunction(Fini, nullptr);
    PIN_StartProgram();
    return 0;
}
