/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2014 Intel Corporation. All rights reserved.  Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/*
 *  This file contains an ISA-portable PIN tool for tracing memory accesses.
 */

#include <stdio.h>
#include <stdlib.h>
#include "pin.H"
#include <set>

#define MAX_NUM_THREADS 32
#define PAGE_SIZE 2048

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", 
        "o", "dirty_pages.out", "specify output file name");
KNOB<double> KnobInsPerSec(KNOB_MODE_WRITEONCE, "pintool", 
        "i", "1e9", "rate of instructions per second for this benchmark");

FILE * out;
UINT64 insPerSec = 0;
PIN_LOCK lock;
PIN_LOCK pages_lock;

INT32 numThreads = 0;
UINT64 lastsum = 0;
UINT64 icount[MAX_NUM_THREADS] = {0};
set<ADDRINT> pages;

// This routine is executed every time a thread is created
VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    PIN_GetLock(&lock, threadid+1);
//    fprintf(out, "thread begin %d\n", threadid);
    fflush(out);
    numThreads++;
    PIN_ReleaseLock(&lock);

    ASSERT(numThreads <= MAX_NUM_THREADS, "Maximum number of threads exceeded\n");
}

// This routine is executed every time a thread is destroyed
VOID ThreadFini(THREADID threadid, const CONTEXT *ctxt, INT32 code, VOID *v)
{
    PIN_GetLock(&lock, threadid+1);
 //   fprintf(out, "thread end %d code %d\n", threadid, code);
    fflush(out);
    PIN_ReleaseLock(&lock);
}

// Pin calls this function every time a new instruction is encountered
VOID PIN_FAST_ANALYSIS_CALL docount(THREADID threadid, ADDRINT c) 
{ 
    UINT64 sum = 0;
    for (INT32 t=0; t<MAX_NUM_THREADS; t++)
        sum += icount[t];
    UINT64 ins = sum - lastsum;

    if ((ins+c) > insPerSec)
    {
        PIN_GetLock(&lock, threadid+1);
        if ((sum-lastsum) > insPerSec)
        {
            fprintf(out, "%lu %lu\n", ins+c, pages.size());
            lastsum = sum;
            PIN_GetLock(&pages_lock, threadid+1);
            pages.clear();
            PIN_ReleaseLock(&pages_lock);
        }
        PIN_ReleaseLock(&lock);
    }

    icount[threadid] += c; 
}

VOID RecordMemWrite(VOID * ip, ADDRINT addr, THREADID threadid)
{
    ADDRINT page_addr = addr/PAGE_SIZE;

    set<ADDRINT>::iterator it; 
    it = pages.find(page_addr);
    if (it == pages.end())
    {
        PIN_GetLock(&pages_lock, threadid+1);
        pages.insert(page_addr);
        PIN_ReleaseLock(&pages_lock);
    }
}

// Pin calls this function every time a new basic block is encountered
// It inserts a call to docount
VOID Trace(TRACE trace, VOID  *v)
{
    // Visit every basic block in the trace
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        // Insert a call to docount for every bbl, passing the number of instructions.
        // IPOINT_ANYWHERE allows Pin to schedule the call anywhere in the bbl to obtain best performance.
        // Use a fast linkage for the call.
        BBL_InsertCall(
                bbl, IPOINT_ANYWHERE, AFUNPTR(docount), IARG_FAST_ANALYSIS_CALL, 
                IARG_THREAD_ID, 
                IARG_UINT32, BBL_NumIns(bbl), 
                IARG_END);
    }
}

// Is called for every instruction and instruments reads and writes
VOID Instruction(INS ins, VOID *v)
{
    // Instruments memory accesses using a predicated call, i.e.
    // the instrumentation is called iff the instruction will actually be executed.
    //
    // On the IA-32 and Intel(R) 64 architectures conditional moves and REP 
    // prefixed instructions appear as predicated instructions in Pin.
    UINT32 memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (UINT32 memOp = 0; memOp < memOperands; memOp++)
    {
        // Note that in some architectures a single memory operand can be 
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)RecordMemWrite,
                IARG_INST_PTR,
                IARG_MEMORYOP_EA, memOp,
                IARG_THREAD_ID,
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID *v)
{
    fclose(out);
    printf("Number of threads ever exist = %d\n", numThreads); 
}

/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
   
INT32 Usage()
{
    PIN_ERROR( "This Pintool prints a trace of memory addresses\n" 
              + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    // Initialize the pin lock
    PIN_InitLock(&lock);

    // Initialize pin
    if (PIN_Init(argc, argv)) return Usage();

    out = fopen(KnobOutputFile.Value().c_str(), "w");
    insPerSec = (UINT64)(KnobInsPerSec.Value()*(double)1e9);

    // Instrumenting functions
    TRACE_AddInstrumentFunction(Trace, 0);
    INS_AddInstrumentFunction(Instruction, 0);

    // Functions to be called when a thread begins/ends
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);

    // Function to be called when the application exits
    PIN_AddFiniFunction(Fini, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
}
