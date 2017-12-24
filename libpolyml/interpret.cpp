/*
    Title:  An interpreter for a compact instruction set.
    Author:     Dave Matthews, Cambridge University Computer Laboratory

    Copyright (c) 2000-7
        Cambridge University Technical Services Limited
    Further development Copyright David C.J. Matthews 2015-17.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x) assert(x)
#else
#define ASSERT(x) 0
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#ifdef HAVE_MATH_H
#include <math.h>
#endif

#include "globals.h"
#include "int_opcodes.h"
#include "machine_dep.h"
#include "sys.h"
#include "profiling.h"
#include "arb.h"
#include "processes.h"
#include "run_time.h"
#include "gc.h"
#include "diagnostics.h"
#include "polystring.h"
#include "save_vec.h"
#include "memmgr.h"
#include "scanaddrs.h"

#if (SIZEOF_VOIDP == 8 && !defined(POLYML32IN64))
#define IS64BITS 1
#endif


#define arg1    (pc[0] + pc[1]*256)
#define arg2    (pc[2] + pc[3]*256)

const PolyWord True = TAGGED(1);
const PolyWord False = TAGGED(0);
const PolyWord Zero = TAGGED(0);

#define CHECKED_REGS 2
#define UNCHECKED_REGS 0

#define EXTRA_STACK 0 // Don't need any extra - signals aren't handled on the Poly stack.

/* the amount of ML stack space to reserve for registers,
   C exception handling etc. The compiler requires us to
   reserve 2 stack-frames worth (2 * 20 words) plus whatever
   we require for the register save area. We actually reserve
   slightly more than this. SPF 3/3/97
*/
#define OVERFLOW_STACK_SIZE \
  (50 + \
   CHECKED_REGS + \
   UNCHECKED_REGS + \
   EXTRA_STACK)


// This duplicates some code in reals.cpp but is now updated.
#define DOUBLESIZE (sizeof(double)/sizeof(POLYUNSIGNED))

union realdb { double dble; POLYUNSIGNED puns[DOUBLESIZE]; };

#define LGWORDSIZE (sizeof(uintptr_t) / sizeof(PolyWord))

union stackItem
{
#ifndef POLYML32IN64
    stackItem(PolyWord v) { words[0] = v; };
    PolyWord words[1];
    bool IsPotentialPtr() const { return true; }
#else
    // In 32-in-64 we need to clear the second PolyWord.  This assumes little-endian.
    stackItem(PolyWord v) { words[0] = v; words[1] = PolyWord::FromUnsigned(0); };
    PolyWord words[2];
    // If the top word is anything other than zero we definitely don't have an address.
    bool IsPotentialPtr() const { return words[1] == PolyWord::FromUnsigned(0); }
#endif
    PolyWord w()const { return words[0]; }
    operator PolyWord () { return words[0]; }
    POLYCODEPTR codeAddr; // Return addresses
    stackItem *stackAddr; // Stack addresses
    intptr_t argValue; // Treat an address as an int
};


class IntTaskData: public TaskData {
public:
    IntTaskData(): interrupt_requested(false), overflowPacket(0), dividePacket(0) {}

    virtual void GarbageCollect(ScanAddress *process);
    void ScanStackAddress(ScanAddress *process, PolyWord &val, StackSpace *stack);
    virtual Handle EnterPolyCode(); // Start running ML

    // Switch to Poly and return with the io function to call.
    int SwitchToPoly();
    virtual void SetException(poly_exn *exc);
    virtual void InterruptCode();

    // AddTimeProfileCount is used in time profiling.
    virtual bool AddTimeProfileCount(SIGNALCONTEXT *context);

    virtual void InitStackFrame(TaskData *newTask, Handle proc, Handle arg);

    // These aren't implemented in the interpreted version.
    virtual Handle EnterCallbackFunction(Handle func, Handle args) { ASSERT(0); return 0; }

    // Increment or decrement the first word of the object pointed to by the
    // mutex argument and return the new value.
    virtual Handle AtomicIncrement(Handle mutexp);
    // Set a mutex to one.
    virtual void AtomicReset(Handle mutexp);

    // Return the minimum space occupied by the stack.   Used when setting a limit.
    virtual uintptr_t currentStackSpace(void) const { return ((stackItem*)this->stack->top - this->taskSp) + OVERFLOW_STACK_SIZE; }

    virtual void addProfileCount(POLYUNSIGNED words) { add_count(this, taskPc, words); }

    virtual void CopyStackFrame(StackObject *old_stack, uintptr_t old_length, StackObject *new_stack, uintptr_t new_length);

    bool interrupt_requested;

    // Allocate memory on the heap.  Returns with the address of the cell. Does not set the
    // length word or any of the data.
    PolyObject *allocateMemory(POLYUNSIGNED words, POLYCODEPTR &pc, stackItem *&sp)
    {
        words++; // Add the size of the length word.
        // N.B. The allocation area may be empty so that both of these are zero.
        if (this->allocPointer >= this->allocLimit + words + 1)
        {
#ifdef POLYML32IN64
            if (words & 1)
            {
                this->allocPointer[-1] = PolyWord::FromUnsigned(0xcccccccc);
                words++;
            }
#endif
            this->allocPointer -= words;
            return (PolyObject *)(this->allocPointer+1);
        }
        // Insufficient space.
        SaveInterpreterState(pc, sp);
        // Find some space to allocate in. Returns a pointer to the newly allocated space.
        // N.B. This may return zero if the heap is exhausted and it has set this
        // up for an exception.  Generally it allocates by decrementing allocPointer
        // but if the required memory is large it may allocate in a separate area.
        PolyWord *space = processes->FindAllocationSpace(this, words, true);
        LoadInterpreterState(pc, sp);
        if (space == 0) return 0;
        return (PolyObject *)(space+1);
    }

    // Put a real result in a "box"
    PolyObject *boxDouble(double d, POLYCODEPTR &pc, stackItem *&sp)
    {
        PolyObject *mem = this->allocateMemory(DOUBLESIZE, pc, sp);
        if (mem == 0) return 0;
        mem->SetLengthWord(DOUBLESIZE, F_BYTE_OBJ);
        union realdb uniondb;
        uniondb.dble = d;
        // Copy the words.  Depending on the word length this may copy one or more words.
        for (unsigned i = 0; i < DOUBLESIZE; i++)
            mem->Set(i, PolyWord::FromUnsigned(uniondb.puns[i]));
        return mem;
    }

    // Extract a double value from a box.
    double unboxDouble(PolyWord p)
    {
        union realdb uniondb;
        for (unsigned i = 0; i < DOUBLESIZE; i++)
            uniondb.puns[i] = p.AsObjPtr()->Get(i).AsUnsigned();
        return uniondb.dble;
    }
    
    // Update the copies in the task object
    void SaveInterpreterState(POLYCODEPTR pc, stackItem *sp)
    {
        taskPc = pc;
        taskSp = sp;
    }

    // Update the local state
    void LoadInterpreterState(POLYCODEPTR &pc, stackItem *&sp)
    {
        pc = taskPc;
        sp = taskSp;
    }

    POLYCODEPTR     taskPc; /* Program counter. */
    stackItem       *taskSp; /* Stack pointer. */
    stackItem       *hr;
    PolyWord        exception_arg;
    bool            raiseException;
    stackItem       *sl; /* Stack limit register. */

    PolyObject      *overflowPacket, *dividePacket;
};

// This lock is used to synchronise all atomic operations.
// It is not needed in the X86 version because that can use a global
// memory lock.
static PLock mutexLock;

// Special value for return address.
#define SPECIAL_PC_END_THREAD           ((POLYCODEPTR)0)

class Interpreter : public MachineDependent {
public:
    Interpreter() {}

    // Create a task data object.
    virtual TaskData *CreateTaskData(void) { return new IntTaskData(); }
    virtual Architectures MachineArchitecture(void) { return MA_Interpreted; }
};

void IntTaskData::InitStackFrame(TaskData *parentTask, Handle proc, Handle arg)
/* Initialise stack frame. */
{
    StackSpace *space = this->stack;
    StackObject *stack = (StackObject *)space->stack();
    PolyObject *closure = DEREFWORDHANDLE(proc);
    uintptr_t stack_size = space->spaceSize() * sizeof(PolyWord) / sizeof(stackItem);
    if (closure->IsClosureObject())
        this->taskPc = *(POLYCODEPTR*)closure;
    else this->taskPc = closure->Get(0).AsCodePtr();

    this->exception_arg = TAGGED(0); /* Used for exception argument. */
    this->taskSp = (stackItem*)stack + stack_size;
    this->raiseException = false;

    /* Set up exception handler */
    /* No previous handler so point it at itself. */
    this->taskSp--;
    this->taskSp->stackAddr = this->taskSp;
    (--this->taskSp)->codeAddr = SPECIAL_PC_END_THREAD; /* Default return address. */
    this->hr = this->taskSp;

    /* If this function takes an argument store it on the stack. */
    if (arg != 0) *(--this->taskSp) = DEREFWORD(arg);

    (*(--this->taskSp)).codeAddr = SPECIAL_PC_END_THREAD; /* Return address. */
    *(--this->taskSp) = (PolyWord)closure; /* Closure address */

    // Make packets for exceptions.             
    overflowPacket = makeExceptionPacket(parentTask, EXC_overflow);
    dividePacket = makeExceptionPacket(parentTask, EXC_divide);
}

extern "C" {
    typedef POLYUNSIGNED(*callFastRts0)();
    typedef POLYUNSIGNED(*callFastRts1)(intptr_t);
    typedef POLYUNSIGNED(*callFastRts2)(intptr_t, intptr_t);
    typedef POLYUNSIGNED(*callFastRts3)(intptr_t, intptr_t, intptr_t);
    typedef POLYUNSIGNED(*callFastRts4)(intptr_t, intptr_t, intptr_t, intptr_t);
    typedef POLYUNSIGNED(*callFastRts5)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
    typedef POLYUNSIGNED(*callFullRts0)(PolyObject *);
    typedef POLYUNSIGNED(*callFullRts1)(PolyObject *, intptr_t);
    typedef POLYUNSIGNED(*callFullRts2)(PolyObject *, intptr_t, intptr_t);
    typedef POLYUNSIGNED(*callFullRts3)(PolyObject *, intptr_t, intptr_t, intptr_t);
    typedef double (*callRTSFtoF) (double);
    typedef double (*callRTSGtoF) (intptr_t);
}

void IntTaskData::InterruptCode()
/* Stop the Poly code at a suitable place. */
/* We may get an asynchronous interrupt at any time. */
{
    IntTaskData *itd = (IntTaskData *)this;
    itd->interrupt_requested = true;
}


void IntTaskData::SetException(poly_exn *exc)
/* Set up the stack of a process to raise an exception. */
{
    this->raiseException = true;
    *(--this->taskSp) = (PolyWord)exc; /* push exception data */
}

int IntTaskData::SwitchToPoly()
/* (Re)-enter the Poly code from C. */
{
    // These are temporary values used where one instruction jumps to
    // common code.
    POLYUNSIGNED    tailCount;
    stackItem       *tailPtr;
    POLYUNSIGNED    returnCount;
    POLYUNSIGNED    storeWords;
    POLYUNSIGNED    stackCheck;
    // Local values.  These are copies of member variables but are used so frequently that
    // it is important that access should be fast.
    POLYCODEPTR     pc;
    stackItem       *sp;

    LoadInterpreterState(pc, sp);

    sl = (stackItem*)((PolyWord*)this->stack->stack() + OVERFLOW_STACK_SIZE);

    // We may have taken an interrupt which has set an exception.
    if (this->raiseException) goto RAISE_EXCEPTION;

    for(;;){ /* Each instruction */
//        char buff[1000];
//        sprintf(buff, "addr = %p sp=%p instr=%02x *sp=%p\n", pc, sp, *pc, (*sp).stackAddr);
//        OutputDebugStringA(buff);

        switch(*pc++) {

        case INSTR_enter_int: pc++; /* Skip the argument. */ break;

        case INSTR_jump8false:
            {
                PolyWord u = *sp++; /* Pop argument */
                if (u == True) { pc += 1; break; }
                /* else - false - take the jump */
            }

        case INSTR_jump8: pc += *pc + 1; break;

        case INSTR_jump16false:
        {
            PolyWord u = *sp++; /* Pop argument */
            if (u == True) { pc += 2; break; }
            /* else - false - take the jump */
        }

        case INSTR_jump16:
            pc += arg1 + 2; break;

        case INSTR_jump32False:
        {
            PolyWord u = *sp++; /* Pop argument */
            if (u == True) { pc += 4; break; }
            /* else - false - take the jump */
        }

        case INSTR_jump32:
        {
            // This is a 32-bit signed quantity on both 64-bits and 32-bits.
            POLYSIGNED offset = pc[3] & 0x80 ? -1 : 0;
            offset = (offset << 8) | pc[3];
            offset = (offset << 8) | pc[2];
            offset = (offset << 8) | pc[1];
            offset = (offset << 8) | pc[0];
            pc += offset + 4;
            break;
        }

        case INSTR_push_handler: /* Save the old handler value. */
            (*(--sp)).stackAddr = this->hr; /* Push old handler */
            break;

        case INSTR_setHandler8: /* Set up a handler */
            (*(--sp)).codeAddr = pc + *pc + 1; /* Address of handler */
            this->hr = sp;
            pc += 1;
            break;

        case INSTR_setHandler16: /* Set up a handler */
            (*(--sp)).codeAddr = pc + arg1 + 2; /* Address of handler */
            this->hr = sp;
            pc += 2;
            break;

        case INSTR_setHandler32: /* Set up a handler */
        {
            POLYUNSIGNED offset = pc[0] + (pc[1] << 8) + (pc[2] << 16) + (pc[3] << 24);
            (*(--sp)).codeAddr = pc + offset + 4; /* Address of handler */
            this->hr = sp;
            pc += 4;
            break;
        }

        case INSTR_deleteHandler: /* Delete handler retaining the result. */
        {
            stackItem u = *sp++;
            sp = this->hr;
            sp++; // Remove handler entry point
            this->hr = (*sp).stackAddr; // Restore old handler
            *sp = u; // Put back the result
            break;
        }

        case INSTR_case16:
            {
                // arg1 is the largest value that is in the range
                POLYSIGNED u = UNTAGGED(*sp++); /* Get the value */
                if (u > arg1 || u < 0) pc += (arg1+2)*2; /* Out of range */
                else {
                    pc += 2;
                    pc += /* Index */pc[u*2]+pc[u*2 + 1]*256; }
                break;
            }

        case INSTR_case32:
        {
            // arg1 is the number of cases i.e. one more than the largest value
            // This is followed by that number of 32-bit offsets.
            // If the value is out of range the default case is immediately after the table.
            POLYSIGNED u = UNTAGGED(*sp++); /* Get the value */
            if (u >= arg1 || u < 0) pc += 2 + arg1 * 4; /* Out of range */
            else
            {
                pc += 2;
                pc += /* Index */pc[u*4] + (pc[u*4+1] << 8) + (pc[u*4+2] << 16) + (pc[u*4+3] << 24);
            }
            break;
        }

        case INSTR_tail_3_b:
           tailCount = 3;
           tailPtr = sp + tailCount;
           sp = tailPtr + *pc;
           goto TAIL_CALL;

        case INSTR_tail_3_2:
           tailCount = 3;
           tailPtr = sp + tailCount;
           sp = tailPtr + 2;
           goto TAIL_CALL;

        case INSTR_tail_3_3:
           tailCount = 3;
           tailPtr = sp + tailCount;
           sp = tailPtr + 3;
           goto TAIL_CALL;

        case INSTR_tail_4_b:
           tailCount = 4;
           tailPtr = sp + tailCount;
           sp = tailPtr + *pc;
           goto TAIL_CALL;

        case INSTR_tail_b_b:
           tailCount = *pc;
           tailPtr = sp + tailCount;
           sp = tailPtr + pc[1];
           goto TAIL_CALL;

        case INSTR_tail:
           /* Tail recursive call. */
           /* Move items up the stack. */
           /* There may be an overlap if the function we are calling
              has more args than this one. */
           tailCount = arg1;
           tailPtr = sp + tailCount;
           sp = tailPtr + arg2;
           TAIL_CALL: /* For general case. */
           if (tailCount < 2) Crash("Invalid argument\n");
           for (; tailCount > 0; tailCount--) *(--sp) = *(--tailPtr);
           pc = (*sp++).codeAddr; /* Pop the original return address. */
           /* And drop through. */

        case INSTR_call_closure: /* Closure call. */
        {
            PolyObject *closure = (*sp).w().AsObjPtr();
            POLYCODEPTR newPc;
            if (closure->IsClosureObject())
                newPc = *(POLYCODEPTR*)closure;
            else
                newPc = (*sp).w().AsObjPtr()->Get(0).AsCodePtr();
            sp--;
            *sp = sp[1];      /* Move closure up. */
            sp[1].codeAddr = pc; /* Save return address. */
            pc = newPc;    /* Get entry point. */
            this->taskPc = pc; // Update in case we're profiling
            break;
        }

        case INSTR_return_w:
            returnCount = arg1; /* Get no. of args to remove. */

            RETURN: /* Common code for return. */
            {
                stackItem result = *sp++; /* Result */
                sp++; /* Remove the link/closure */
                pc = (*sp++).codeAddr; /* Return address */
                sp += returnCount; /* Add on number of args. */
                if (pc == SPECIAL_PC_END_THREAD)
                    exitThread(this); // This thread is exiting.
                *(--sp) = result; /* Result */
                this->taskPc = pc; // Update in case we're profiling
            }
            break;

        case INSTR_return_b: returnCount = *pc; goto RETURN;
        case INSTR_return_0: returnCount = 0; goto RETURN;
        case INSTR_return_1: returnCount = 1; goto RETURN;
        case INSTR_return_2: returnCount = 2; goto RETURN;
        case INSTR_return_3: returnCount = 3; goto RETURN;

        case INSTR_stackSize8:
            stackCheck = *pc++;
            goto STACKCHECK;

        case INSTR_stackSize16:
        {
            stackCheck = arg1; pc += 2;
        STACKCHECK:
            // Check there is space on the stack
            if (sp - stackCheck < sl)
            {
                uintptr_t min_size = (this->stack->top - (PolyWord*)sp) + OVERFLOW_STACK_SIZE + stackCheck;
                SaveInterpreterState(pc, sp);
                CheckAndGrowStack(this, min_size);
                LoadInterpreterState(pc, sp);
                sl = (stackItem*)this->stack->stack() + OVERFLOW_STACK_SIZE;
            }
            // Also check for interrupts
            if (this->interrupt_requested)
            {
                // Check for interrupts
                this->interrupt_requested = false;
                SaveInterpreterState(pc, sp);
                return -1;
            }
            break;
        }

        case INSTR_pad: /* No-op */ break;

        case INSTR_raise_ex:
            {
                RAISE_EXCEPTION:
                this->raiseException = false;
                PolyException *exn = (PolyException*)((*sp).w().AsObjPtr());
                this->exception_arg = exn; /* Get exception data */
                sp = this->hr;
                pc = (*sp++).codeAddr;
                if (pc == SPECIAL_PC_END_THREAD)
                    exitThread(this);  // Default handler for thread.
                this->hr = (*sp++).stackAddr;
                break;
            }

        case INSTR_get_store_w:
        // Get_store is now only used for mutually recursive closures.  It allocates mutable store
        // initialised to zero.
        {
            storeWords = arg1;
            pc += 2;
            GET_STORE:
            PolyObject *p = this->allocateMemory(storeWords, pc, sp);
            if (p == 0) goto RAISE_EXCEPTION;
            p->SetLengthWord(storeWords, F_MUTABLE_BIT);
            for(; storeWords > 0; ) p->Set(--storeWords, TAGGED(0)); /* Must initialise store! */
            *(--sp) = (PolyWord)p;
            break;
        }

        case INSTR_get_store_2: storeWords = 2; goto GET_STORE;
        case INSTR_get_store_3: storeWords = 3; goto GET_STORE;
        case INSTR_get_store_4: storeWords = 4; goto GET_STORE;
        case INSTR_get_store_b: storeWords = *pc; pc++; goto GET_STORE;

        case INSTR_tuple_w:
        {
            storeWords = arg1; pc += 2;
        TUPLE: /* Common code for tupling. */
            PolyObject *p = this->allocateMemory(storeWords, pc, sp);
            if (p == 0) goto RAISE_EXCEPTION; // Exception
            p->SetLengthWord(storeWords, 0);
            for(; storeWords > 0; ) p->Set(--storeWords, *sp++);
            *(--sp) = (PolyWord)p;
            break;
        }

        case INSTR_tuple_2: storeWords = 2; goto TUPLE;
        case INSTR_tuple_3: storeWords = 3; goto TUPLE;
        case INSTR_tuple_4: storeWords = 4; goto TUPLE;
        case INSTR_tuple_b: storeWords = *pc; pc++; goto TUPLE;

        case INSTR_local_w:
            {
                stackItem u = sp[arg1];
                *(--sp) = u;
                pc += 2;
                break;
            }

        case INSTR_indirect_w:
            *sp = (*sp).w().AsObjPtr()->Get(arg1); pc += 2; break;

        case INSTR_indContainerW:
            *sp = (*sp).stackAddr[arg1]; pc += 2; break;

        case INSTR_move_to_vec_w:
            {
                PolyWord u = *sp++;
                (*sp).w().AsObjPtr()->Set(arg1, u);
                pc += 2;
                break;
            }

        case INSTR_set_container_w:
        {
            PolyWord u = *sp++;
            (*sp).stackAddr[arg1] = u;
            pc += 2;
            break;
        }

        case INSTR_set_stack_val_w:
            {
                stackItem u = *sp++;
                sp[arg1-1] = u;
                pc += 2;
                break;
            }

        case INSTR_reset_w: sp += arg1; pc += 2; break;

        case INSTR_reset_r_w:
            {
                stackItem u = *sp;
                sp += arg1;
                *sp = u;
                pc += 2;
                break;
            }

        case INSTR_constAddr8:
            *(--sp) = *(PolyWord*)(pc + pc[0] + 1); pc += 1; break;

        case INSTR_constAddr16:
            *(--sp) = *(PolyWord*)(pc + arg1 + 2); pc += 2; break;

        case INSTR_constAddr32:
        {
            POLYUNSIGNED offset = pc[0] + (pc[1] << 8) + (pc[2] << 16) + (pc[3] << 24);
            *(--sp) = *(PolyWord*)(pc + offset + 4);
            pc += 4;
            break;
        }

        case INSTR_const_int_w: *(--sp) = TAGGED(arg1); pc += 2; break;

        case INSTR_jump_back8:
            pc -= *pc + 1;
            if (this->interrupt_requested)
            {
                // Check for interrupt in case we're in a loop
                this->interrupt_requested = false;
                SaveInterpreterState(pc, sp);
                return -1;
            }
            break;

        case INSTR_jump_back16:
            pc -= arg1 + 1;
            if (this->interrupt_requested)
            {
                // Check for interrupt in case we're in a loop
                this->interrupt_requested = false;
                SaveInterpreterState(pc, sp);
                return -1;
            }
            break;

        case INSTR_lock:
            {
                PolyObject *obj = (*sp).w().AsObjPtr();
                obj->SetLengthWord(obj->LengthWord() & ~_OBJ_MUTABLE_BIT);
                break;
            }

        case INSTR_ldexc: *(--sp) = this->exception_arg; break;

        case INSTR_local_b: { stackItem u = sp[*pc]; *(--sp) = u; pc += 1; break; }

        case INSTR_indirect_b:
            *sp = (*sp).w().AsObjPtr()->Get(*pc); pc += 1; break;

        case INSTR_indContainerB:
            *sp = (*sp).stackAddr[*pc]; pc += 1; break;

        case INSTR_move_to_vec_b:
            { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(*pc, u); pc += 1; break; }

        case INSTR_set_container_b:
        { PolyWord u = *sp++; (*sp).stackAddr[*pc] = u; pc += 1; break; }

        case INSTR_set_stack_val_b:
            { stackItem u = *sp++; sp[*pc-1] = u; pc += 1; break; }

        case INSTR_reset_b: sp += *pc; pc += 1; break;

        case INSTR_reset_r_b:
            { stackItem u = *sp; sp += *pc; *sp = u; pc += 1; break; }

        case INSTR_const_int_b: *(--sp) = TAGGED(*pc); pc += 1; break;

        case INSTR_setclosure:
        {
            PolyObject *srcPtr = (*sp++).w().AsObjPtr();
            // Temporarily we may actually pass the code address here or
            // we may pass an old-format closure.
            if (!srcPtr->IsCodeObject())
            {
                if (srcPtr->IsClosureObject())
                    srcPtr = *(PolyObject**)srcPtr;
                else srcPtr = srcPtr->Get(0).AsObjPtr();
            }
            PolyObject **destPtr = (PolyObject **)(*sp).w().AsObjPtr();
            *destPtr = srcPtr;
            break;
        }

        case INSTR_local_0: { stackItem u = sp[0]; *(--sp) = u; break; }
        case INSTR_local_1: { stackItem u = sp[1]; *(--sp) = u; break; }
        case INSTR_local_2: { stackItem u = sp[2]; *(--sp) = u; break; }
        case INSTR_local_3: { stackItem u = sp[3]; *(--sp) = u; break; }
        case INSTR_local_4: { stackItem u = sp[4]; *(--sp) = u; break; }
        case INSTR_local_5: { stackItem u = sp[5]; *(--sp) = u; break; }
        case INSTR_local_6: { stackItem u = sp[6]; *(--sp) = u; break; }
        case INSTR_local_7: { stackItem u = sp[7]; *(--sp) = u; break; }
        case INSTR_local_8: { stackItem u = sp[8]; *(--sp) = u; break; }
        case INSTR_local_9: { stackItem u = sp[9]; *(--sp) = u; break; }
        case INSTR_local_10: { stackItem u = sp[10]; *(--sp) = u; break; }
        case INSTR_local_11: { stackItem u = sp[11]; *(--sp) = u; break; }

        case INSTR_indirect_0:
            *sp = (*sp).w().AsObjPtr()->Get(0); break;

        case INSTR_indirect_1:
            *sp = (*sp).w().AsObjPtr()->Get(1); break;

        case INSTR_indirect_2:
            *sp = (*sp).w().AsObjPtr()->Get(2); break;

        case INSTR_indirect_3:
            *sp = (*sp).w().AsObjPtr()->Get(3); break;

        case INSTR_indirect_4:
            *sp = (*sp).w().AsObjPtr()->Get(4); break;

        case INSTR_indirect_5:
            *sp = (*sp).w().AsObjPtr()->Get(5); break;

        case INSTR_indContainer0:
            *sp = (*sp).stackAddr[0]; break;

        case INSTR_indContainer1:
            *sp = (*sp).stackAddr[1]; break;

        case INSTR_indContainer2:
            *sp = (*sp).stackAddr[2]; break;

        case INSTR_const_0: *(--sp) = Zero; break;
        case INSTR_const_1: *(--sp) = TAGGED(1); break;
        case INSTR_const_2: *(--sp) = TAGGED(2); break;
        case INSTR_const_3: *(--sp) = TAGGED(3); break;
        case INSTR_const_4: *(--sp) = TAGGED(4); break;
        case INSTR_const_10: *(--sp) = TAGGED(10); break;

            // Move-to-vec is now only used for closures.
        case INSTR_move_to_vec_0: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(0, u); break; }
        case INSTR_move_to_vec_1: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(1, u); break; }
        case INSTR_move_to_vec_2: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(2, u); break; }
        case INSTR_move_to_vec_3: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(3, u); break; }
        case INSTR_move_to_vec_4: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(4, u); break; }
        case INSTR_move_to_vec_5: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(5, u); break; }
        case INSTR_move_to_vec_6: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(6, u); break; }
        case INSTR_move_to_vec_7: { PolyWord u = *sp++; (*sp).w().AsObjPtr()->Set(7, u); break; }

        case INSTR_set_container_0: { PolyWord u = *sp++; (*sp).stackAddr[0] = u; break; }
        case INSTR_set_container_1: { PolyWord u = *sp++; (*sp).stackAddr[1] = u; break; }
        case INSTR_set_container_2: { PolyWord u = *sp++; (*sp).stackAddr[2] = u; break; }
        case INSTR_set_container_3: { PolyWord u = *sp++; (*sp).stackAddr[3] = u; break; }
        case INSTR_set_container_4: { PolyWord u = *sp++; (*sp).stackAddr[4] = u; break; }

        case INSTR_reset_r_1: { stackItem u = *sp; sp += 1; *sp = u; break; }
        case INSTR_reset_r_2: { stackItem u = *sp; sp += 2; *sp = u; break; }
        case INSTR_reset_r_3: { stackItem u = *sp; sp += 3; *sp = u; break; }

        case INSTR_reset_1: sp += 1; break;
        case INSTR_reset_2: sp += 2; break;

        case INSTR_stack_container:
        {
            POLYUNSIGNED words = arg1; pc += 2;
            while (words-- > 0) *(--sp) = Zero;
            sp--;
            (*sp).stackAddr = sp+1;
            break;
        }

        case INSTR_tuple_container: /* Create a tuple from a container. */
            {
                storeWords = arg1;
                PolyObject *t = this->allocateMemory(storeWords, pc, sp);
                if (t == 0) goto RAISE_EXCEPTION;
                t->SetLengthWord(storeWords, 0);
                for(; storeWords > 0; )
                {
                    storeWords--;
                    t->Set(storeWords, (*sp).stackAddr[storeWords]);
                }
                *sp = (PolyWord)t;
                pc += 2;
                break;
            }

        case INSTR_callFastRTS0:
            {
                callFastRts0 doCall = *(callFastRts0*)(*sp++).w().AsObjPtr();
                POLYUNSIGNED result = doCall();
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS1:
            {
                callFastRts1 doCall = *(callFastRts1*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg1 = (*sp++).argValue;
                POLYUNSIGNED result = doCall(rtsArg1);
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS2:
            {
                callFastRts2 doCall = *(callFastRts2*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg2 = (*sp++).argValue; // Pop off the args, last arg first.
                intptr_t rtsArg1 = (*sp++).argValue;
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2);
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS3:
            {
                callFastRts3 doCall = *(callFastRts3*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg3 = (*sp++).argValue; // Pop off the args, last arg first.
                intptr_t rtsArg2 = (*sp++).argValue;
                intptr_t rtsArg1 = (*sp++).argValue;
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2, rtsArg3);
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS4:
            {
                callFastRts4 doCall = *(callFastRts4*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg4 = (*sp++).argValue; // Pop off the args, last arg first.
                intptr_t rtsArg3 = (*sp++).argValue;
                intptr_t rtsArg2 = (*sp++).argValue;
                intptr_t rtsArg1 = (*sp++).argValue;
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2, rtsArg3, rtsArg4);
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS5:
            {
                callFastRts5 doCall = *(callFastRts5*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg5 = (*sp++).argValue; // Pop off the args, last arg first.
                intptr_t rtsArg4 = (*sp++).argValue;
                intptr_t rtsArg3 = (*sp++).argValue;
                intptr_t rtsArg2 = (*sp++).argValue;
                intptr_t rtsArg1 = (*sp++).argValue;
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2, rtsArg3, rtsArg4, rtsArg5);
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS0:
            {
                callFullRts0 doCall = *(callFullRts0*)(*sp++).w().AsObjPtr();
                this->raiseException = false;
                SaveInterpreterState(pc, sp);
                POLYUNSIGNED result = doCall(this->threadObject);
                LoadInterpreterState(pc, sp);
                // If this raised an exception 
                if (this->raiseException) goto RAISE_EXCEPTION;
                *(--sp)= PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS1:
            {
                callFullRts1 doCall = *(callFullRts1*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg1 = (*sp++).argValue;
                this->raiseException = false;
                SaveInterpreterState(pc, sp);
                POLYUNSIGNED result = doCall(this->threadObject, rtsArg1);
                LoadInterpreterState(pc, sp);
                // If this raised an exception 
                if (this->raiseException) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS2:
            {
                callFullRts2 doCall = *(callFullRts2*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg2 = (*sp++).argValue; // Pop off the args, last arg first.
                intptr_t rtsArg1 = (*sp++).argValue;
                this->raiseException = false;
                SaveInterpreterState(pc, sp);
                POLYUNSIGNED result = doCall(this->threadObject, rtsArg1, rtsArg2);
                LoadInterpreterState(pc, sp);
                // If this raised an exception 
                if (this->raiseException) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS3:
            {
                callFullRts3 doCall = *(callFullRts3*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg3 = (*sp++).argValue; // Pop off the args, last arg first.
                intptr_t rtsArg2 = (*sp++).argValue;
                intptr_t rtsArg1 = (*sp++).argValue;
                this->raiseException = false;
                SaveInterpreterState(pc, sp);
                POLYUNSIGNED result = doCall(this->threadObject, rtsArg1, rtsArg2, rtsArg3);
                LoadInterpreterState(pc, sp);
                // If this raised an exception 
                if (this->raiseException) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastFtoF:
            {
                // Floating point call.  The call itself does not allocate but we
                // need to put the result into a "box".
                callRTSFtoF doCall = *(callRTSFtoF*)(*sp++).w().AsObjPtr();
                PolyWord rtsArg1 = *sp++;
                double argument = unboxDouble(rtsArg1);
                // Allocate memory for the result.
                double result = doCall(argument);
                PolyObject *t = boxDouble(result, pc, sp);
                if (t == 0) goto RAISE_EXCEPTION;
                *(--sp) = (PolyWord)t;
                break;
            }

        case INSTR_callFastGtoF:
            {
                // Call that takes a POLYUNSIGNED argument and returns a double.
                callRTSGtoF doCall = *(callRTSGtoF*)(*sp++).w().AsObjPtr();
                intptr_t rtsArg1 = (*sp++).argValue;
                // Allocate memory for the result.
                double result = doCall(rtsArg1);
                PolyObject *t = boxDouble(result, pc, sp);
                if (t == 0) goto RAISE_EXCEPTION;
                *(--sp) = (PolyWord)t;
                break;
            }

        case INSTR_notBoolean:
            *sp = ((*sp).w() == True) ? False : True; break;

        case INSTR_isTagged:
            *sp = (*sp).w().IsTagged() ? True : False; break;

        case INSTR_cellLength:
            /* Return the length word. */
            *sp = TAGGED((*sp).w().AsObjPtr()->Length());
            break;

        case INSTR_cellFlags:
        {
            PolyObject *p = (*sp).w().AsObjPtr();
            POLYUNSIGNED f = (p->LengthWord()) >> OBJ_PRIVATE_FLAGS_SHIFT;
            *sp = TAGGED(f);
            break;
        }

        case INSTR_clearMutable:
        {
            PolyObject *obj = (*sp).w().AsObjPtr();
            POLYUNSIGNED lengthW = obj->LengthWord();
            /* Clear the mutable bit. */
            obj->SetLengthWord(lengthW & ~_OBJ_MUTABLE_BIT);
            *sp = Zero;
            break;
        }

        case INSTR_stringLength: // Now replaced by loadUntagged
            *sp = TAGGED(((PolyStringObject*)(*sp).w().AsObjPtr())->length);
            break;

        case INSTR_atomicIncr:
        {
            PLocker l(&mutexLock);
            PolyObject *p = (*sp).w().AsObjPtr();
            PolyWord newValue = TAGGED(UNTAGGED(p->Get(0))+1);
            p->Set(0, newValue);
            *sp = newValue;
            break;
        }

        case INSTR_atomicDecr:
        {
            PLocker l(&mutexLock);
            PolyObject *p = (*sp).w().AsObjPtr();
            PolyWord newValue = TAGGED(UNTAGGED(p->Get(0))-1);
            p->Set(0, newValue);
            *sp = newValue;
            break;
        }

        case INSTR_atomicReset:
        {
            // This is needed in the interpreted version otherwise there
            // is a chance that we could set the value to zero while another
            // thread is between getting the old value and setting it to the new value.
            PLocker l(&mutexLock);
            PolyObject *p = (*sp).w().AsObjPtr();
            p->Set(0, TAGGED(1)); // Set this to released.
            *sp = TAGGED(0); // Push the unit result
            break;
        }

        case INSTR_longWToTagged:
        {
            // Extract the first word and return it as a tagged value.  This loses the top-bit
            POLYUNSIGNED wx = (*sp).w().AsObjPtr()->Get(0).AsUnsigned();
            *sp = TAGGED(wx);
            break;
        }

        case INSTR_signedToLongW:
        {
            // Shift the tagged value to remove the tag and put it into the first word.
            // The original sign bit is copied in the shift.
            intptr_t wx = (*sp).w().UnTagged();
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(intptr_t*)t = wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_unsignedToLongW:
        {
            // As with the above except the value is treated as an unsigned
            // value and the top bit is zero.
            uintptr_t wx = (*sp).w().UnTaggedUnsigned();
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_realAbs:
        {
            PolyObject *t = this->boxDouble(fabs(unboxDouble(*sp)), pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_realNeg:
        {
            PolyObject *t = this->boxDouble(-(unboxDouble(*sp)), pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_floatFixedInt:
        {
            POLYSIGNED u = UNTAGGED(*sp);
            PolyObject *t = this->boxDouble((double)u, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_equalWord:
        {
            PolyWord u = *sp++;
            *sp = u == (*sp).w() ? True : False;
            break;
        }

        case INSTR_lessSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsSigned() < u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_lessUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsUnsigned() < u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_lessEqSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsSigned() <= u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_lessEqUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsUnsigned() <= u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_greaterSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsSigned() > u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_greaterUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsUnsigned() > u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_greaterEqSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsSigned() >= u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_greaterEqUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).w().AsUnsigned() >= u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_fixedAdd:
        {
            PolyWord x = *sp++;
            PolyWord y = (*sp).w();
            POLYSIGNED t = UNTAGGED(x) + UNTAGGED(y);
            if (t <= MAXTAGGED && t >= -MAXTAGGED-1)
                *sp = TAGGED(t);
            else
            {
                *(--sp) = (PolyWord)overflowPacket;
                goto RAISE_EXCEPTION;
            }
            break;
        }

        case INSTR_fixedSub:
        {
            PolyWord x = *sp++;
            PolyWord y = (*sp).w();
            POLYSIGNED t = UNTAGGED(y) - UNTAGGED(x);
            if (t <= MAXTAGGED && t >= -MAXTAGGED-1)
                *sp = TAGGED(t);
            else
            {
                *(--sp) = (PolyWord)overflowPacket;
                goto RAISE_EXCEPTION;
            }
            break;
        }

        case INSTR_fixedMult:
        {
            // We need to detect overflow.  There doesn't seem to be any convenient way to do
            // this so we use the arbitrary precision package and check whether the result is short.
            // Clang and GCC 5.0 have __builtin_mul_overflow which will do this but GCC 5.0 is not
            // currently (July 2016) in Debian stable.
            Handle reset = this->saveVec.mark();
            Handle pushedArg1 = this->saveVec.push(*sp++);
            Handle pushedArg2 = this->saveVec.push(*sp);
            Handle result = mult_longc(this, pushedArg2, pushedArg1);
            PolyWord res = result->Word();
            this->saveVec.reset(reset);
            if (! res.IsTagged()) 
            {
                *(--sp) = (PolyWord)overflowPacket;
                goto RAISE_EXCEPTION;
            }
            *sp = res;
            break;
        }

        case INSTR_fixedQuot:
        {
            // Zero and overflow are checked for in ML.
            POLYSIGNED u = UNTAGGED(*sp++);
            PolyWord y = (*sp).w();
            *sp = TAGGED(UNTAGGED(y) / u);
            break;
        }

        case INSTR_fixedRem:
        {
            // Zero and overflow are checked for in ML.
            POLYSIGNED u = UNTAGGED(*sp++);
            PolyWord y = (*sp).w();
            *sp = TAGGED(UNTAGGED(y) % u);
            break;
        }

        case INSTR_wordAdd:
        {
            PolyWord u = *sp++;
            // Because we're not concerned with overflow we can just add the values and subtract the tag.
            *sp = PolyWord::FromUnsigned((*sp).w().AsUnsigned() + u.AsUnsigned() - TAGGED(0).AsUnsigned());
            break;
        }

        case INSTR_wordSub:
        {
            PolyWord u = *sp++;
            *sp = PolyWord::FromUnsigned((*sp).w().AsUnsigned() - u.AsUnsigned() + TAGGED(0).AsUnsigned());
            break;
        }

        case INSTR_wordMult:
        {
            PolyWord u = *sp++;
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) * UNTAGGED_UNSIGNED(u));
            break;
        }

        case INSTR_wordDiv:
        {
            POLYUNSIGNED u = UNTAGGED_UNSIGNED(*sp++);
            // Detection of zero is done in ML
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) / u); break;
        }

        case INSTR_wordMod:
        {
            POLYUNSIGNED u = UNTAGGED_UNSIGNED(*sp++);
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) % u);
            break;
        }

        case INSTR_wordAnd:
        {
            PolyWord u = *sp++;
            // Since both of these should be tagged the tag bit will be preserved.
            *sp = PolyWord::FromUnsigned((*sp).w().AsUnsigned() & u.AsUnsigned());
            break;
        }

        case INSTR_wordOr:
        {
            PolyWord u = *sp++;
            // Since both of these should be tagged the tag bit will be preserved.
            *sp = PolyWord::FromUnsigned((*sp).w().AsUnsigned() | u.AsUnsigned());
            break;
        }

        case INSTR_wordXor:
        {
            PolyWord u = *sp++;
            // This will remove the tag bit so it has to be reinstated.
            *sp = PolyWord::FromUnsigned(((*sp).w().AsUnsigned() ^ u.AsUnsigned()) | TAGGED(0).AsUnsigned());
            break;
        }

        case INSTR_wordShiftLeft:
        {
            // ML requires shifts greater than a word to return zero. 
            // That's dealt with at the higher level.
            PolyWord u = *sp++;
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) << UNTAGGED_UNSIGNED(u));
            break;
        }

        case INSTR_wordShiftRLog:
        {
            PolyWord u = *sp++;
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) >> UNTAGGED_UNSIGNED(u));
            break;
        }

        case INSTR_wordShiftRArith:
        {
            PolyWord u = *sp++;
            // Strictly speaking, C does not require that this uses
            // arithmetic shifting so we really ought to set the
            // high-order bits explicitly.
            *sp = TAGGED(UNTAGGED(*sp) >> UNTAGGED(u));
            break;
        }

        case INSTR_allocByteMem:
        {
            // Allocate byte segment.  This does not need to be initialised.
            POLYUNSIGNED flags = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp);
            PolyObject *t = this->allocateMemory(length, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION; // Exception
            t->SetLengthWord(length, (byte)flags);
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordEqual:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            *sp = wx == wy ? True : False;
            break;
        }

        case INSTR_lgWordNotequal:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            *sp = wx != wy ? True : False;
            break;
        }

        case INSTR_lgWordLess:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            *sp = (wy < wx) ? True : False;
            break;
        }

        case INSTR_lgWordLessEq:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            *sp = (wy <= wx) ? True : False;
            break;
        }

        case INSTR_lgWordGreater:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            *sp = (wy > wx) ? True : False;
            break;
        }

        case INSTR_lgWordGreaterEq:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            *sp = (wy >= wx) ? True : False;
            break;
        }

        case INSTR_lgWordAdd:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy+wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordSub:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy-wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordMult:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy*wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordDiv:
         {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy/wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordMod:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy%wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordAnd:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy&wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordOr:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy|wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordXor:
        {
            uintptr_t wx = *(uintptr_t*)((*sp++).w().AsObjPtr());
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy^wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordShiftLeft:
        {
            // The shift amount is a tagged word not a boxed large word
            POLYUNSIGNED wx = UNTAGGED_UNSIGNED(*sp++);
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy << wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordShiftRLog:
        {
            // The shift amount is a tagged word not a boxed large word
            POLYUNSIGNED wx = UNTAGGED_UNSIGNED(*sp++);
            uintptr_t wy = *(uintptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = wy >> wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_lgWordShiftRArith:
        {
            // The shift amount is a tagged word not a boxed large word
            POLYUNSIGNED wx = UNTAGGED_UNSIGNED(*sp++);
            intptr_t wy = *(intptr_t*)((*sp).w().AsObjPtr());
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(intptr_t*)t = wy >> wx;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_realEqual:
        {
            double u = unboxDouble(*sp++);
            *sp = u == unboxDouble(*sp) ? True: False;
            break;
        }

        case INSTR_realLess:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) < u ? True: False;
            break;
        }

        case INSTR_realLessEq:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) <= u ? True: False;
            break;
        }

        case INSTR_realGreater:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) > u ? True: False;
            break;
        }

        case INSTR_realGreaterEq:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) >= u ? True: False;
            break;
        }

        case INSTR_realAdd:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v+u, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_realSub:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v-u, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_realMult:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v*u, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_realDiv:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v/u, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_getThreadId:
            *(--sp) = (PolyWord)this->threadObject;
            break;

        case INSTR_allocWordMemory:
        {
            // Allocate word segment.  This must be initialised.
            // We mustn't pop the initialiser until after any potential GC.
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(sp[2].w());
            PolyObject *t = this->allocateMemory(length, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            PolyWord initialiser = *sp++;
            POLYUNSIGNED flags = UNTAGGED_UNSIGNED(*sp++);
            t->SetLengthWord(length, (byte)flags);
            *sp = (PolyWord)t;
            // Have to initialise the data.
            for (; length > 0; ) t->Set(--length, initialiser);
            break;
        }

        case INSTR_alloc_ref:
        {
            // Allocate a single word mutable cell.  This is more common than allocWordMemory on its own.
            PolyObject *t = this->allocateMemory(1, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            PolyWord initialiser = (*sp).w();
            t->SetLengthWord(1, F_MUTABLE_BIT);
            t->Set(0, initialiser);
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_loadMLWord:
        {
            // The values on the stack are base, index and offset.
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).w().AsCodePtr() + offset);
            *sp = p->Get(index);
            break;
        }

        case INSTR_loadMLByte:
        {
            // The values on the stack are base and index.
            POLYUNSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).w().AsCodePtr();
            *sp = TAGGED(p[index]); // Have to tag the result
            break;
        }

        case INSTR_loadC8:
        {
            // This is similar to loadMLByte except that the base address is a boxed large-word.
            // Also the index is SIGNED.
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr()));
            *sp = TAGGED(p[index]); // Have to tag the result
            break;
        }

        case INSTR_loadC16:
        {
            // This and the other loads are similar to loadMLWord with separate
            // index and offset values.
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            POLYUNSIGNED r = ((uint16_t*)p)[index];
            *sp = TAGGED(r);
            break;
        }

        case INSTR_loadC32:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            uintptr_t r = ((uint32_t*)p)[index];
#ifdef IS64BITS
            // This is tagged in 64-bit mode
            *sp = TAGGED(r);
#else
            // But boxed in 32-bit mode.
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = r;
            *sp = (PolyWord)t;
#endif
            break;
        }

#if (defined(IS64BITS) || defined(POLYML32IN64))
        case INSTR_loadC64:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            uintptr_t r = ((uint64_t*)p)[index];
            // This must be boxed.
            PolyObject *t = this->allocateMemory(LGWORDSIZE, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(LGWORDSIZE, F_BYTE_OBJ);
            *(uintptr_t*)t = r;
            *sp = (PolyWord)t;
            break;
        }
#endif

        case INSTR_loadCFloat:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            double r = ((float*)p)[index];
            // This must be boxed.
            PolyObject *t = this->boxDouble(r, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_loadCDouble:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            double r = ((double*)p)[index];
            // This must be boxed.
            PolyObject *t = this->boxDouble(r, pc, sp);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = (PolyWord)t;
            break;
        }

        case INSTR_loadUntagged:
        {
            // The values on the stack are base, index and offset.
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).w().AsCodePtr() + offset);
            *sp = TAGGED(p->Get(index).AsUnsigned());
            break;
        }

        case INSTR_storeMLWord: 
        {
            PolyWord toStore = *sp++;
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).w().AsCodePtr() + offset);
            p->Set(index, toStore);
            *sp = Zero;
            break;
        }

        case INSTR_storeMLByte: 
        {
            POLYUNSIGNED toStore = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).w().AsCodePtr();
            p[index] = (byte)toStore;
            *sp = Zero;
            break; 
        }

        case INSTR_storeC8: 
        {
            // Similar to storeMLByte except that the base address is a boxed large-word.
            POLYUNSIGNED toStore = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr()));
            p[index] = (byte)toStore;
            *sp = Zero;
            break; 
        }

        case INSTR_storeC16:
        {
            uint16_t toStore = (uint16_t)UNTAGGED(*sp++);
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            ((uint16_t*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

        case INSTR_storeC32:
        {
#ifdef IS64BITS
            // This is a tagged value in 64-bit mode.
            uint32_t toStore = (uint32_t)UNTAGGED(*sp++);
#else
            // but a boxed value in 32-bit mode.
            uint32_t toStore = (uint32_t)(*(uintptr_t*)((*sp++).w().AsObjPtr()));
#endif
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            ((uint32_t*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

#if (defined(IS64BITS) || defined(POLYML32IN64))
        case INSTR_storeC64:
        {
            // This is a boxed value.
            uint64_t toStore = *(uintptr_t*)((*sp++).w().AsObjPtr());
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            ((uint64_t*)p)[index] = toStore;
            *sp = Zero;
            break;
        }
#endif

        case INSTR_storeCFloat:
        {
            // This is a boxed value.
            float toStore = (float)unboxDouble(*sp++);
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            ((float*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

        case INSTR_storeCDouble:
        {
            // This is a boxed value.
            double toStore = unboxDouble(*sp++);
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = *((byte **)((*sp).w().AsObjPtr())) + offset;
            ((double*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

        case INSTR_storeUntagged: 
        {
            PolyWord toStore = PolyWord::FromUnsigned(UNTAGGED_UNSIGNED(*sp++));
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).w().AsCodePtr() + offset);
            p->Set(index, toStore);
            *sp = Zero;
            break;
        }

        case INSTR_blockMoveWord:
        {
            // The offsets are byte counts but the the indexes are in words.
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED destOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED destIndex = UNTAGGED_UNSIGNED(*sp++);
            PolyObject *dest = (PolyObject*)((*sp++).w().AsCodePtr() + destOffset);
            POLYUNSIGNED srcOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED srcIndex = UNTAGGED_UNSIGNED(*sp++);
            PolyObject *src = (PolyObject*)((*sp).w().AsCodePtr() + srcOffset);
            for (POLYUNSIGNED u = 0; u < length; u++) dest->Set(destIndex+u, src->Get(srcIndex+u));
            *sp = Zero;
            break;
        }

        case INSTR_blockMoveByte:
        {
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED destOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR dest = (*sp++).w().AsCodePtr();
            POLYUNSIGNED srcOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR src = (*sp).w().AsCodePtr();
            memcpy(dest+destOffset, src+srcOffset, length);
            *sp = Zero;
            break;
        }

        case INSTR_blockEqualByte:
        {
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED arg2Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg2Ptr = (*sp++).w().AsCodePtr();
            POLYUNSIGNED arg1Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg1Ptr = (*sp).w().AsCodePtr();
            *sp = memcmp(arg1Ptr+arg1Offset, arg2Ptr+arg2Offset, length) == 0 ? True : False;
            break;
        }

        case INSTR_blockCompareByte:
        {
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED arg2Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg2Ptr = (*sp++).w().AsCodePtr();
            POLYUNSIGNED arg1Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg1Ptr = (*sp).w().AsCodePtr();
            int result = memcmp(arg1Ptr+arg1Offset, arg2Ptr+arg2Offset, length);
            *sp = result == 0 ? TAGGED(0) : result < 0 ? TAGGED(-1) : TAGGED(1);
            break;
        }

        default: Crash("Unknown instruction %x\n", pc[-1]);

        } /* switch */
     } /* for */
     return 0;
} /* MD_switch_to_poly */

void IntTaskData::GarbageCollect(ScanAddress *process)
{
    TaskData::GarbageCollect(process);

    overflowPacket = process->ScanObjectAddress(overflowPacket);
    dividePacket = process->ScanObjectAddress(dividePacket);

    if (stack != 0)
    {
        StackSpace *stackSpace = stack;
        stackItem *stackPtr = this->taskSp;
        // The exception arg if any
        ScanStackAddress(process, this->exception_arg, stackSpace);

        // Now the values on the stack.
        for (stackItem *q = stackPtr; q < (stackItem*)stackSpace->top; q++)
        {
            if (q->IsPotentialPtr())
            {
                PolyWord w = q->w();
                if (w.IsDataPtr())
                {
                    ScanStackAddress(process, w, stackSpace);
                    *q = w;
                }
            }
        }
     }
}


// Process a value within the stack.
void IntTaskData::ScanStackAddress(ScanAddress *process, PolyWord &val, StackSpace *stack)
{
    if (! val.IsDataPtr()) return;

    MemSpace *space = gMem.LocalSpaceForAddress(val.AsStackAddr()-1);
    if (space != 0)
        val = process->ScanObjectAddress(val.AsObjPtr());
}


// Copy a stack.  The lengths are numbers of PolyWords 
void IntTaskData::CopyStackFrame(StackObject *old_stack, uintptr_t old_length, StackObject *new_stack, uintptr_t new_length)
{
    /* Moves a stack, updating all references within the stack */
#ifdef POLYML32IN64
    old_length = old_length / 2;
    new_length = new_length / 2;
#endif
    stackItem *old_base  = (stackItem *)old_stack;
    stackItem *new_base  = (stackItem*)new_stack;
    stackItem *old_top   = old_base + old_length;

    /* Calculate the offset of the new stack from the old. If the frame is
       being extended objects in the new frame will be further up the stack
       than in the old one. */

    uintptr_t offset = new_base - old_base + new_length - old_length;
    stackItem *oldSp = this->taskSp;
    this->taskSp = oldSp + offset;
    this->hr    = this->hr + offset;

    /* Skip the unused part of the stack. */

    uintptr_t i = oldSp - (stackItem*)old_base;

    ASSERT (i <= old_length);

    i = old_length - i;

    stackItem *old = oldSp;
    stackItem *newp = this->taskSp;

    while (i--)
    {
        stackItem old_word = *old++;
        if (old_word.w().IsDataPtr() && old_word.stackAddr >= old_base && old_word.stackAddr <= old_top)
            old_word.stackAddr = old_word.stackAddr + offset;
        else if (old_word.w().IsDataPtr() && IsHeapAddress(old_word.stackAddr))
        {
            stackItem *addr = (stackItem*)old_word.w().AsStackAddr();
            if (addr >= old_base && addr <= old_top)
            {
                addr += offset;
                old_word = PolyWord::FromStackAddr((PolyWord*)addr);
            }
        }
        *newp++ = old_word;
    }
    ASSERT(old == ((stackItem*)old_stack)+old_length);
    ASSERT(newp == ((stackItem*)new_stack)+new_length);
}

Handle IntTaskData::EnterPolyCode()
/* Called from "main" to enter the code. */
{
    Handle hOriginal = this->saveVec.mark(); // Set this up for the IO calls.
    while (1)
    {
        this->saveVec.reset(hOriginal); // Remove old RTS arguments and results.

        // Run the ML code and return with the function to call.
        this->inML = true;
        int ioFunction = SwitchToPoly();
        this->inML = false;

        try {
            switch (ioFunction)
            {
            case -1:
                // We've been interrupted.  This usually involves simulating a
                // stack overflow so we could come here because of a genuine
                // stack overflow.
                // Previously this code was executed on every RTS call but there
                // were problems on Mac OS X at least with contention on schedLock.
                // Process any asynchronous events i.e. interrupts or kill
                processes->ProcessAsynchRequests(this);
                // Release and re-acquire use of the ML memory to allow another thread
                // to GC.
                processes->ThreadReleaseMLMemory(this);
                processes->ThreadUseMLMemory(this);
                break;

            case -2: // A callback has returned.
                ASSERT(0); // Callbacks aren't implemented

            default:
                Crash("Unknown io operation %d\n", ioFunction);
            }
        }
        catch (IOException &) {
        }

    }
}

// As far as possible we want locking and unlocking an ML mutex to be fast so
// we try to implement the code in the assembly code using appropriate
// interlocked instructions.  That does mean that if we need to lock and
// unlock an ML mutex in this code we have to use the same, machine-dependent,
// code to do it.  These are defaults that are used where there is no
// machine-specific code.

static Handle ProcessAtomicIncrement(TaskData *taskData, Handle mutexp)
{
    PLocker l(&mutexLock);
    PolyObject *p = DEREFHANDLE(mutexp);
    // A thread can only call this once so the values will be short
    PolyWord newValue = TAGGED(UNTAGGED(p->Get(0))+1);
    p->Set(0, newValue);
    return taskData->saveVec.push(newValue);
}

// Release a mutex.  We need to lock the mutex to ensure we don't
// reset it in the time between one of atomic operations reading
// and writing the mutex.
static Handle ProcessAtomicReset(TaskData *taskData, Handle mutexp)
{
    PLocker l(&mutexLock);
    DEREFHANDLE(mutexp)->Set(0, TAGGED(1)); // Set this to released.
    return taskData->saveVec.push(TAGGED(0)); // Push the unit result
}

Handle IntTaskData::AtomicIncrement(Handle mutexp)
{
    return ProcessAtomicIncrement(this, mutexp);
}

void IntTaskData::AtomicReset(Handle mutexp)
{
    (void)ProcessAtomicReset(this, mutexp);
}

bool IntTaskData::AddTimeProfileCount(SIGNALCONTEXT *context)
{
    if (taskPc != 0)
    {
        // See if the PC we've got is an ML code address.
        MemSpace *space = gMem.SpaceForAddress(taskPc);
        if (space != 0 && (space->spaceType == ST_CODE || space->spaceType == ST_PERMANENT))
        {
            add_count(this, taskPc, 1);
            return true;
        }
    }
    return false;
}


static Interpreter interpreterObject;

MachineDependent *machineDependent = &interpreterObject;
