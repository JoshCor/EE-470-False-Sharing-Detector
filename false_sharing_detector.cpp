#include "pin.H"
#include <iostream>

// Called when the target program exits
VOID Fini(INT32 code, VOID* v) {
    std::cerr << "[detector] program finished\n";
}

int main(int argc, char* argv[]) {
    PIN_Init(argc, argv);
    PIN_AddFiniFunction(Fini, nullptr);
    PIN_StartProgram();
    return 0;
}