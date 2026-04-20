#include "pin.H"
#include <iostream>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <unordered_set>


// access record data
struct AccessRecord {
    ADDRINT  cache_line;
    ADDRINT  addr;
    THREADID tid;
    bool     is_write;
    ADDRINT  ip;
};


// array of access records for each thread
#define MAX_THREADS 64
static std::vector<AccessRecord> thread_records[MAX_THREADS];

//package data and put in the Access record
VOID RecordAccess(VOID* addr, BOOL is_write, ADDRINT ip, THREADID tid) {
    AccessRecord rec;
    rec.addr       = (ADDRINT)addr;
    rec.cache_line = (ADDRINT)addr & ~63ULL;
    rec.is_write   = is_write;
    rec.ip         = ip;
    rec.tid        = tid;
    thread_records[tid].push_back(rec);
}

//decide when to instrument instructions (on reads and writes and pass necessary data along)
VOID Instruction(INS ins, VOID* v) {
    //write
    if (INS_IsMemoryWrite(ins)) {
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)RecordAccess,
            IARG_MEMORYWRITE_EA,    // effective address of the write
            IARG_BOOL, TRUE,        // is_write = true
            IARG_INST_PTR,          // instruction pointer
            IARG_THREAD_ID,         // PIN thread ID
            IARG_END
        );
    }

    //read
    if (INS_IsMemoryRead(ins)) {
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)RecordAccess,
            IARG_MEMORYREAD_EA,     // effective address of the read
            IARG_BOOL, FALSE,       // is_write = false
            IARG_INST_PTR,          // instruction pointer
            IARG_THREAD_ID,         // PIN thread ID
            IARG_END
        );
    }
}

// report after finish
VOID Fini(INT32 code, VOID* v) {
    std::cerr << "\n[detector] instrumentation complete\n";

    size_t total = 0;
    for (int t = 0; t < MAX_THREADS; t++) {
        if (!thread_records[t].empty()) {
            std::cerr << "  thread " << t
                      << ": " << thread_records[t].size()
                      << " accesses\n";
            total += thread_records[t].size();
        }
    }
    std::cerr << "  total: " << total << " accesses\n";
}


int main(int argc, char* argv[]) {
    PIN_Init(argc, argv);

    //set up instrument functions
    INS_AddInstrumentFunction(Instruction, nullptr);

    //report
    PIN_AddFiniFunction(Fini, nullptr);

    // Hand control to PIN — never returns
    PIN_StartProgram();
    return 0;
}