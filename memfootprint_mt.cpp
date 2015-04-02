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
#include <map>

#define MAX_NUM_THREADS 8

PIN_LOCK lock;
PIN_LOCK bytes_lock;

INT32 numThreads = 0;

struct ADDRSTAT {
    UINT32 accesses;
    UINT32 all_bytes_read;
    UINT32 smallest_byte_read;
    UINT32 largest_byte_read;
    BOOL is_read;
    BOOL is_write;
};

map <ADDRINT, ADDRSTAT> addrs[MAX_NUM_THREADS];
UINT64 thread_all_bytes_read[MAX_NUM_THREADS] = {0};

//FILE * trace;
FILE** trace_files = (FILE **)malloc(sizeof(FILE*) * (MAX_NUM_THREADS));

VOID CountBytes(ADDRINT addr, UINT32 size, THREADID t, BOOL l, BOOL s)
{
    thread_all_bytes_read[t] += size;
    map <ADDRINT, ADDRSTAT>::iterator itr = addrs[t].find(addr);
    if (itr == addrs[t].end()) {
        ADDRSTAT new_stat = {1, size, size, size, l, s};
        addrs[t][addr] = new_stat;
    }
    else {
        (itr->second).accesses++;
        (itr->second).all_bytes_read += size;
        if (!(itr->second).is_read)
            (itr->second).is_read = l;
        if (!(itr->second).is_write)
            (itr->second).is_write = s;
        if ((itr->second).smallest_byte_read > size)
            (itr->second).smallest_byte_read = size;
        if ((itr->second).largest_byte_read < size)
            (itr->second).largest_byte_read = size;
    }
}

// Print a memory read record
VOID RecordMemRead(VOID * ip, ADDRINT addr, UINT32 size, THREADID threadid)
{
    //fprintf(trace_files[threadid],"%p: R %lu %u\n", ip, addr, size);
    //PIN_GetLock(&bytes_lock, threadid+1);
    CountBytes(addr, size, threadid, 1, 0);
    //PIN_ReleaseLock(&bytes_lock);
}

// Print a memory write record
VOID RecordMemWrite(VOID * ip, ADDRINT addr, UINT32 size, THREADID threadid)
{
    //fprintf(trace_files[threadid],"%p: W %lu %u\n", ip, addr, size);
    //PIN_GetLock(&bytes_lock, threadid+1);
    CountBytes(addr, size, threadid, 0, 1);
    //PIN_ReleaseLock(&bytes_lock);
}

VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    PIN_InitLock(&lock);
    PIN_GetLock(&lock, threadid+1);
    numThreads++;
    PIN_ReleaseLock(&lock);

    ASSERT(numThreads <= MAX_NUM_THREADS, "Maximum number of threads exceeded\n");
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
        const UINT32 size = INS_MemoryOperandSize(ins, memOp);

        if (INS_MemoryOperandIsRead(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)RecordMemRead,
                IARG_INST_PTR,
                IARG_MEMORYOP_EA, memOp,
                IARG_UINT32, size, 
                IARG_THREAD_ID, 
                IARG_END);
        }
        // Note that in some architectures a single memory operand can be 
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)RecordMemWrite,
                IARG_INST_PTR,
                IARG_MEMORYOP_EA, memOp,
                IARG_UINT32, size, 
                IARG_THREAD_ID,
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID *v)
{
    printf("Number of threads ever exist = %d\n", numThreads); 


    //fprintf(trace, "#eof\n");
    //fclose(trace);
    
    for (INT32 t=0; t<MAX_NUM_THREADS; t++)
    {
        //fprintf(trace_files[t], "#eof\n");
        fclose(trace_files[t]);
    }


    UINT64 total_all_bytes_read = 0;
    UINT64 total_addrs = 0;
    for (INT32 t=0; t<MAX_NUM_THREADS; t++)
    {
        printf("Thread %d addrs %lu all_bytes_read %lu\n", t, addrs[t].size(), thread_all_bytes_read[t]);
        total_addrs += addrs[t].size();
        total_all_bytes_read += thread_all_bytes_read[t];
    }

    UINT32 num_read_only_addrs = 0;
    map <ADDRINT, ADDRSTAT>::iterator itr = addrs[0].begin();
    while (itr != addrs[0].end())
    {
        if ((itr->second).is_read && !(itr->second).is_write)
            num_read_only_addrs++;
        itr++;
    }

    printf("Total addrs %lu\n", total_addrs);
    printf("Read-only addrs %u\n", num_read_only_addrs);
    printf("Total all_bytes_read %lu\n", total_all_bytes_read);

    free(trace_files);
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
    // Initialize pin
    if (PIN_Init(argc, argv)) return Usage();

    //trace = fopen("pinatrace.out", "w");
    for (INT32 t=0; t<MAX_NUM_THREADS; t++)
    {
        char buffer[25];
        snprintf(buffer, sizeof(char) * 20, "memfootprint_%d.out", t);
        trace_files[t] = fopen(buffer, "w");
    }

    PIN_InitLock(&lock);
    PIN_InitLock(&bytes_lock);
    PIN_AddThreadStartFunction(ThreadStart, 0);

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
}
