// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
#include "common.h"
#include "CommonTypes.h"
#include "CommonMacros.h"
#include "daccess.h"
#include "PalRedhawkCommon.h"
#include "PalRedhawk.h"
#include "RedhawkWarnings.h"
#include "rhassert.h"
#include "slist.h"
#include "gcrhinterface.h"
#include "varint.h"
#include "regdisplay.h"
#include "StackFrameIterator.h"
#include "thread.h"
#include "holder.h"
#include "Crst.h"
#include "RWLock.h"
#include "event.h"
#include "threadstore.h"
#include "stressLog.h"

#include "shash.h"
#include "module.h"
#include "RuntimeInstance.h"
#include "rhbinder.h"

// warning C4061: enumerator '{blah}' in switch of enum '{blarg}' is not explicitly handled by a case label
#pragma warning(disable:4061)

#if !defined(CORERT) // @TODO: CORERT: these are (currently) only implemented in assembly helpers
// When we use a thunk to call out to managed code from the runtime the following label is the instruction
// immediately following the thunk's call instruction. As such it can be used to identify when such a callout
// has occured as we are walking the stack.
EXTERN_C void * ReturnFromManagedCallout2;
GVAL_IMPL_INIT(PTR_VOID, g_ReturnFromManagedCallout2Addr, &ReturnFromManagedCallout2);

#if defined(FEATURE_DYNAMIC_CODE)
EXTERN_C void * ReturnFromUniversalTransition;
GVAL_IMPL_INIT(PTR_VOID, g_ReturnFromUniversalTransitionAddr, &ReturnFromUniversalTransition);

EXTERN_C void * ReturnFromCallDescrThunk;
GVAL_IMPL_INIT(PTR_VOID, g_ReturnFromCallDescrThunkAddr, &ReturnFromCallDescrThunk);
#endif

#ifdef _TARGET_X86_
EXTERN_C void * RhpCallFunclet2;
GVAL_IMPL_INIT(PTR_VOID, g_RhpCallFunclet2Addr, &RhpCallFunclet2);
#endif
EXTERN_C void * RhpCallCatchFunclet2;
GVAL_IMPL_INIT(PTR_VOID, g_RhpCallCatchFunclet2Addr, &RhpCallCatchFunclet2);
EXTERN_C void * RhpCallFinallyFunclet2;
GVAL_IMPL_INIT(PTR_VOID, g_RhpCallFinallyFunclet2Addr, &RhpCallFinallyFunclet2);
EXTERN_C void * RhpCallFilterFunclet2;
GVAL_IMPL_INIT(PTR_VOID, g_RhpCallFilterFunclet2Addr, &RhpCallFilterFunclet2);
EXTERN_C void * RhpThrowEx2;
GVAL_IMPL_INIT(PTR_VOID, g_RhpThrowEx2Addr, &RhpThrowEx2);
EXTERN_C void * RhpThrowHwEx2;
GVAL_IMPL_INIT(PTR_VOID, g_RhpThrowHwEx2Addr, &RhpThrowHwEx2);
EXTERN_C void * RhpRethrow2;
GVAL_IMPL_INIT(PTR_VOID, g_RhpRethrow2Addr, &RhpRethrow2);
#endif //!defined(CORERT)

// Addresses of functions in the DAC won't match their runtime counterparts so we
// assign them to globals. However it is more performant in the runtime to compare
// against immediates than to fetch the global. This macro hides the difference.
#ifdef DACCESS_COMPILE
#define EQUALS_CODE_ADDRESS(x, func_name) ((x) == g_ ## func_name ## Addr)
#else
#define EQUALS_CODE_ADDRESS(x, func_name) ((x) == &func_name)
#endif

#ifdef DACCESS_COMPILE
#define FAILFAST_OR_DAC_FAIL(x) if(!(x)) { DacError(E_FAIL); }
#define FAILFAST_OR_DAC_FAIL_MSG(x, msg) if(!(x)) { DacError(E_FAIL); }
#define FAILFAST_OR_DAC_FAIL_UNCONDITIONALLY(msg) DacError(E_FAIL)
#else
#define FAILFAST_OR_DAC_FAIL(x) if(!(x)) { ASSERT_UNCONDITIONALLY(#x); RhFailFast(); }
#define FAILFAST_OR_DAC_FAIL_MSG(x, msg) if(!(x)) { ASSERT_MSG((x), msg); ASSERT_UNCONDITIONALLY(#x); RhFailFast(); }
#define FAILFAST_OR_DAC_FAIL_UNCONDITIONALLY(msg) { ASSERT_UNCONDITIONALLY(msg); RhFailFast(); }
#endif

// The managed callout thunk above stashes a transition frame pointer in its FP frame. The following constant
// is the offset from the FP at which this pointer is stored.
#define MANAGED_CALLOUT_THUNK_TRANSITION_FRAME_POINTER_OFFSET (-(Int32)sizeof(UIntNative))

PTR_PInvokeTransitionFrame GetPInvokeTransitionFrame(PTR_VOID pTransitionFrame)
{
    return static_cast<PTR_PInvokeTransitionFrame>(pTransitionFrame);
}


StackFrameIterator::StackFrameIterator(Thread * pThreadToWalk, PTR_VOID pInitialTransitionFrame)
{
    STRESS_LOG0(LF_STACKWALK, LL_INFO10000, "----Init---- [ GC ]\n");
    ASSERT(!pThreadToWalk->DangerousCrossThreadIsHijacked());
    InternalInit(pThreadToWalk, GetPInvokeTransitionFrame(pInitialTransitionFrame), GcStackWalkFlags);
    PrepareToYieldFrame();
}

StackFrameIterator::StackFrameIterator(Thread * pThreadToWalk, PTR_PAL_LIMITED_CONTEXT pCtx)
{
    STRESS_LOG0(LF_STACKWALK, LL_INFO10000, "----Init---- [ hijack ]\n");
    InternalInit(pThreadToWalk, pCtx, 0);
    PrepareToYieldFrame();
}

void StackFrameIterator::ResetNextExInfoForSP(UIntNative SP)
{
    while (m_pNextExInfo && (SP > (UIntNative)dac_cast<TADDR>(m_pNextExInfo)))
        m_pNextExInfo = m_pNextExInfo->m_pPrevExInfo;
}

void StackFrameIterator::EnterInitialInvalidState(Thread * pThreadToWalk)
{
    m_pThread = pThreadToWalk;
    m_pInstance = GetRuntimeInstance();
    m_pCodeManager = NULL;
    m_pHijackedReturnValue = NULL;
    m_HijackedReturnValueKind = GCRK_Unknown;
    m_pConservativeStackRangeLowerBound = NULL;
    m_pConservativeStackRangeUpperBound = NULL;
    m_pendingFuncletFramePointer = NULL;
    m_pNextExInfo = pThreadToWalk->GetCurExInfo();
    m_ControlPC = 0;
}

// Prepare to start a stack walk from the context listed in the supplied PInvokeTransitionFrame.
// The supplied frame can be TOP_OF_STACK_MARKER to indicate that there are no more managed
// frames on the stack.  Otherwise, the context in the frame always describes a callsite
// where control transitioned from managed to unmanaged code.
// NOTE: When a return address hijack is executed, the PC in the generated PInvokeTransitionFrame
// matches the hijacked return address.  This PC is not guaranteed to be in managed code
// since the hijacked return address may refer to a location where an assembly thunk called
// into managed code.
// NOTE: When the PC is in an assembly thunk, this function will unwind to the next managed
// frame and may publish a conservative stack range (if and only if any of the unwound
// thunks report a conservative range).
void StackFrameIterator::InternalInit(Thread * pThreadToWalk, PTR_PInvokeTransitionFrame pFrame, UInt32 dwFlags)
{
    // EH stackwalks are always required to unwind non-volatile floating point state.  This
    // state is never carried by PInvokeTransitionFrames, implying that they can never be used
    // as the initial state for an EH stackwalk.
    ASSERT_MSG(!(dwFlags & ApplyReturnAddressAdjustment), 
        "PInvokeTransitionFrame content is not sufficient to seed an EH stackwalk");

    EnterInitialInvalidState(pThreadToWalk);

    if (pFrame == TOP_OF_STACK_MARKER)
    {
        // There are no managed frames on the stack.  Leave the iterator in its initial invalid state.
        return;
    }

    m_dwFlags = dwFlags;

    // We need to walk the ExInfo chain in parallel with the stackwalk so that we know when we cross over 
    // exception throw points.  So we must find our initial point in the ExInfo chain here so that we can 
    // properly walk it in parallel.
    ResetNextExInfoForSP((UIntNative)dac_cast<TADDR>(pFrame));

    memset(&m_RegDisplay, 0, sizeof(m_RegDisplay));
    m_RegDisplay.SetIP((PCODE)pFrame->m_RIP);
    m_RegDisplay.SetAddrOfIP((PTR_PCODE)PTR_HOST_MEMBER(PInvokeTransitionFrame, pFrame, m_RIP));
    m_ControlPC = dac_cast<PTR_VOID>(*(m_RegDisplay.pIP));

    PTR_UIntNative pPreservedRegsCursor = (PTR_UIntNative)PTR_HOST_MEMBER(PInvokeTransitionFrame, pFrame, m_PreservedRegs);

#ifdef _TARGET_ARM_
    m_RegDisplay.pLR = (PTR_UIntNative)PTR_HOST_MEMBER(PInvokeTransitionFrame, pFrame, m_RIP);
    m_RegDisplay.pR11 = (PTR_UIntNative)PTR_HOST_MEMBER(PInvokeTransitionFrame, pFrame, m_ChainPointer);
     
    if (pFrame->m_dwFlags & PTFF_SAVE_R4)  { m_RegDisplay.pR4 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R5)  { m_RegDisplay.pR5 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R6)  { m_RegDisplay.pR6 = pPreservedRegsCursor++; }
    ASSERT(!(pFrame->m_dwFlags & PTFF_SAVE_R7)); // R7 should never contain a GC ref because we require
                                                 // a frame pointer for methods with pinvokes
    if (pFrame->m_dwFlags & PTFF_SAVE_R8)  { m_RegDisplay.pR8 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R9)  { m_RegDisplay.pR9 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R10)  { m_RegDisplay.pR10 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_SP)  { m_RegDisplay.SP  = *pPreservedRegsCursor++; }

    m_RegDisplay.pR7 = (PTR_UIntNative) PTR_HOST_MEMBER(PInvokeTransitionFrame, pFrame, m_FramePointer);

    if (pFrame->m_dwFlags & PTFF_SAVE_R0)  { m_RegDisplay.pR0 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R1)  { m_RegDisplay.pR1 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R2)  { m_RegDisplay.pR2 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R3)  { m_RegDisplay.pR3 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_LR)  { m_RegDisplay.pLR = pPreservedRegsCursor++; }

    if (pFrame->m_dwFlags & PTFF_R0_IS_GCREF)
    {
        m_pHijackedReturnValue = (PTR_RtuObjectRef) m_RegDisplay.pR0;
        m_HijackedReturnValueKind = GCRK_Object;
    }
    if (pFrame->m_dwFlags & PTFF_R0_IS_BYREF)
    {
        m_pHijackedReturnValue = (PTR_RtuObjectRef) m_RegDisplay.pR0;
        m_HijackedReturnValueKind = GCRK_Byref;
    }

#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");

#else // _TARGET_ARM_
    if (pFrame->m_dwFlags & PTFF_SAVE_RBX)  { m_RegDisplay.pRbx = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_RSI)  { m_RegDisplay.pRsi = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_RDI)  { m_RegDisplay.pRdi = pPreservedRegsCursor++; }
    ASSERT(!(pFrame->m_dwFlags & PTFF_SAVE_RBP)); // RBP should never contain a GC ref because we require
                                                  // a frame pointer for methods with pinvokes
#ifdef _TARGET_AMD64_
    if (pFrame->m_dwFlags & PTFF_SAVE_R12)  { m_RegDisplay.pR12 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R13)  { m_RegDisplay.pR13 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R14)  { m_RegDisplay.pR14 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R15)  { m_RegDisplay.pR15 = pPreservedRegsCursor++; }
#endif // _TARGET_AMD64_

    m_RegDisplay.pRbp = (PTR_UIntNative) PTR_HOST_MEMBER(PInvokeTransitionFrame, pFrame, m_FramePointer);

    if (pFrame->m_dwFlags & PTFF_SAVE_RSP)  { m_RegDisplay.SP   = *pPreservedRegsCursor++; }

    if (pFrame->m_dwFlags & PTFF_SAVE_RAX)  { m_RegDisplay.pRax = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_RCX)  { m_RegDisplay.pRcx = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_RDX)  { m_RegDisplay.pRdx = pPreservedRegsCursor++; }
#ifdef _TARGET_AMD64_
    if (pFrame->m_dwFlags & PTFF_SAVE_R8 )  { m_RegDisplay.pR8  = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R9 )  { m_RegDisplay.pR9  = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R10)  { m_RegDisplay.pR10 = pPreservedRegsCursor++; }
    if (pFrame->m_dwFlags & PTFF_SAVE_R11)  { m_RegDisplay.pR11 = pPreservedRegsCursor++; }
#endif // _TARGET_AMD64_

    if (pFrame->m_dwFlags & PTFF_RAX_IS_GCREF)
    {
        m_pHijackedReturnValue = (PTR_RtuObjectRef) m_RegDisplay.pRax;
        m_HijackedReturnValueKind = GCRK_Object;
    }
    if (pFrame->m_dwFlags & PTFF_RAX_IS_BYREF)
    {
        m_pHijackedReturnValue = (PTR_RtuObjectRef) m_RegDisplay.pRax;
        m_HijackedReturnValueKind = GCRK_Byref;
    }

#endif // _TARGET_ARM_

    // @TODO: currently, we always save all registers -- how do we handle the onese we don't save once we 
    //        start only saving those that weren't already saved?

    // This function guarantees that the final initialized context will refer to a managed
    // frame.  In the rare case where the PC does not refer to managed code (and refers to an
    // assembly thunk instead), unwind through the thunk sequence to find the nearest managed
    // frame.
    // NOTE: When thunks are present, the thunk sequence may report a conservative GC reporting
    // lower bound that must be applied when processing the managed frame.

    ReturnAddressCategory category = CategorizeUnadjustedReturnAddress(m_ControlPC);

    if (category == InManagedCode)
    {
        ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC));
    }
    else if (IsNonEHThunk(category))
    {
        UnwindNonEHThunkSequence();
        ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC));
    }
    else
    {
        FAILFAST_OR_DAC_FAIL_UNCONDITIONALLY("PInvokeTransitionFrame PC points to an unexpected assembly thunk kind.");
    }

    STRESS_LOG1(LF_STACKWALK, LL_INFO10000, "   %p\n", m_ControlPC);
}

#ifndef DACCESS_COMPILE

void StackFrameIterator::InternalInitForEH(Thread * pThreadToWalk, PAL_LIMITED_CONTEXT * pCtx)
{
    STRESS_LOG0(LF_STACKWALK, LL_INFO10000, "----Init---- [ EH ]\n");
    InternalInit(pThreadToWalk, pCtx, EHStackWalkFlags);
    PrepareToYieldFrame();
    STRESS_LOG1(LF_STACKWALK, LL_INFO10000, "   %p\n", m_ControlPC);
}

void StackFrameIterator::InternalInitForStackTrace()
{
    STRESS_LOG0(LF_STACKWALK, LL_INFO10000, "----Init---- [ StackTrace ]\n");
    Thread * pThreadToWalk = ThreadStore::GetCurrentThread();
    PTR_VOID pFrame = pThreadToWalk->GetTransitionFrameForStackTrace();
    InternalInit(pThreadToWalk, GetPInvokeTransitionFrame(pFrame), StackTraceStackWalkFlags);
    PrepareToYieldFrame();
}

#endif //!DACCESS_COMPILE

// Prepare to start a stack walk from the context listed in the supplied PAL_LIMITED_CONTEXT.
// The supplied context can describe a location in either managed or unmanaged code.  In the
// latter case the iterator is left in an invalid state when this function returns.
void StackFrameIterator::InternalInit(Thread * pThreadToWalk, PTR_PAL_LIMITED_CONTEXT pCtx, UInt32 dwFlags)
{
    ASSERT((dwFlags & MethodStateCalculated) == 0);

    EnterInitialInvalidState(pThreadToWalk);

    m_dwFlags = dwFlags;

    // We need to walk the ExInfo chain in parallel with the stackwalk so that we know when we cross over 
    // exception throw points.  So we must find our initial point in the ExInfo chain here so that we can 
    // properly walk it in parallel.
    ResetNextExInfoForSP(pCtx->GetSp());

    // This codepath is used by the hijack stackwalk and we can get arbitrary ControlPCs from there.  If this
    // context has a non-managed control PC, then we're done.
    if (!m_pInstance->FindCodeManagerByAddress(dac_cast<PTR_VOID>(pCtx->GetIp())))
        return;

    //
    // control state
    //
    m_ControlPC       = dac_cast<PTR_VOID>(pCtx->GetIp());
    m_RegDisplay.SP   = pCtx->GetSp();
    m_RegDisplay.IP   = pCtx->GetIp();
    m_RegDisplay.pIP  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, IP);

#ifdef _TARGET_ARM_
    //
    // preserved regs
    //
    m_RegDisplay.pR4  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R4);
    m_RegDisplay.pR5  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R5);
    m_RegDisplay.pR6  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R6);
    m_RegDisplay.pR7  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R7);
    m_RegDisplay.pR8  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R8);
    m_RegDisplay.pR9  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R9);
    m_RegDisplay.pR10 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R10);
    m_RegDisplay.pR11 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R11);
    m_RegDisplay.pLR  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, LR);

    //
    // preserved vfp regs
    //
    for (Int32 i = 0; i < 16 - 8; i++)
    {
        m_RegDisplay.D[i] = pCtx->D[i];
    }
    //
    // scratch regs
    //
    m_RegDisplay.pR0  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R0);

#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");

#else // _TARGET_ARM_
    //
    // preserved regs
    //
    m_RegDisplay.pRbp = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, Rbp);
    m_RegDisplay.pRsi = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, Rsi);
    m_RegDisplay.pRdi = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, Rdi);
    m_RegDisplay.pRbx = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, Rbx);
#ifdef _TARGET_AMD64_
    m_RegDisplay.pR12 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R12);
    m_RegDisplay.pR13 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R13);
    m_RegDisplay.pR14 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R14);
    m_RegDisplay.pR15 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, R15);
    //
    // preserved xmm regs
    //
    memcpy(m_RegDisplay.Xmm, &pCtx->Xmm6, sizeof(m_RegDisplay.Xmm));
#endif // _TARGET_AMD64_

    //
    // scratch regs
    //
    m_RegDisplay.pRax = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pCtx, Rax);
    m_RegDisplay.pRcx = NULL;
    m_RegDisplay.pRdx = NULL;
#ifdef _TARGET_AMD64_
    m_RegDisplay.pR8  = NULL;
    m_RegDisplay.pR9  = NULL;
    m_RegDisplay.pR10 = NULL;
    m_RegDisplay.pR11 = NULL;
#endif // _TARGET_AMD64_
#endif // _TARGET_ARM_
}

PTR_VOID StackFrameIterator::HandleExCollide(PTR_ExInfo pExInfo)
{
    STRESS_LOG3(LF_STACKWALK, LL_INFO10000, "   [ ex collide ] kind = %d, pass = %d, idxCurClause = %d\n", 
                pExInfo->m_kind, pExInfo->m_passNumber, pExInfo->m_idxCurClause);

    PTR_VOID collapsingTargetFrame = NULL;
    UInt32 curFlags = m_dwFlags;

    // Capture and clear the pending funclet frame pointer (if any).  This field is only set
    // when stack walks collide with active exception dispatch, and only exists to save the
    // funclet frame pointer until the next ExInfo collision (which has now occurred).
    PTR_VOID activeFuncletFramePointer = m_pendingFuncletFramePointer;
    m_pendingFuncletFramePointer = NULL;

    // If we aren't invoking a funclet (i.e. idxCurClause == -1), and we're doing a GC stackwalk, we don't 
    // want the 2nd-pass collided behavior because that behavior assumes that the previous frame was a 
    // funclet, which isn't the case when taking a GC at some points in the EH dispatch code.  So we treat it
    // as if the 2nd pass hasn't actually started yet.
    if ((pExInfo->m_passNumber == 1) || 
        (pExInfo->m_idxCurClause == 0xFFFFFFFF)) 
    {
        FAILFAST_OR_DAC_FAIL_MSG(!(curFlags & ApplyReturnAddressAdjustment),
            "did not expect to collide with a 1st-pass ExInfo during a EH stackwalk");
        InternalInit(m_pThread, pExInfo->m_pExContext, curFlags);
        m_pNextExInfo = pExInfo->m_pPrevExInfo;
        CalculateCurrentMethodState();
        ASSERT(IsValid());

        if ((pExInfo->m_kind == EK_HardwareFault) && (curFlags & RemapHardwareFaultsToSafePoint))
            GetCodeManager()->RemapHardwareFaultToGCSafePoint(&m_methodInfo, &m_codeOffset);
    }
    else
    {
        ASSERT_MSG(activeFuncletFramePointer != NULL,
            "collided with an active funclet invoke but the funclet frame pointer is unknown");

        //
        // Copy our state from the previous StackFrameIterator
        //
        this->UpdateFromExceptionDispatch((PTR_StackFrameIterator)&pExInfo->m_frameIter);

        // Sync our 'current' ExInfo with the updated state (we may have skipped other dispatches)
        ResetNextExInfoForSP(m_RegDisplay.GetSP());

        if ((m_dwFlags & ApplyReturnAddressAdjustment) && (curFlags & ApplyReturnAddressAdjustment))
        {
            // Counteract our pre-adjusted m_ControlPC, since the caller of this routine will apply the 
            // adjustment again once we return.
            m_ControlPC = AdjustReturnAddressForward(m_ControlPC);
        }
        m_dwFlags = curFlags;

        // The iterator has been moved to the "owner frame" (either a parent funclet or the main
        // code body) of the funclet being invoked by this ExInfo.  As a result, both the active
        // funclet and the current frame must be "part of the same function" and therefore must
        // have identical frame pointer values.

        CalculateCurrentMethodState();
        ASSERT(IsValid());
        ASSERT(m_FramePointer == activeFuncletFramePointer);

        if ((m_ControlPC != 0) &&           // the dispatch in ExInfo could have gone unhandled
            (m_dwFlags & CollapseFunclets))
        {
            // GC stack walks must skip the owner frame since GC information for the entire function
            // has already been reported by the leafmost active funclet.  In general, the GC stack walk
            // must skip all parent frames that are "part of the same function" (i.e., have the same
            // frame pointer).
            collapsingTargetFrame = activeFuncletFramePointer;
        }
    }
    return collapsingTargetFrame;
}

void StackFrameIterator::UpdateFromExceptionDispatch(PTR_StackFrameIterator pSourceIterator)
{
    ASSERT(m_pendingFuncletFramePointer == NULL);
    PreservedRegPtrs thisFuncletPtrs = this->m_funcletPtrs;

    // Blast over 'this' with everything from the 'source'.  
    *this = *pSourceIterator;

    // Clear the funclet frame pointer (if any) that was loaded from the previous iterator.
    // This field does not relate to the transferrable state of the previous iterator (it
    // instead tracks the frame-by-frame progression of a particular iterator instance) and
    // therefore has no meaning in the context of the current stack walk.
    m_pendingFuncletFramePointer = NULL;

    // Then, put back the pointers to the funclet's preserved registers (since those are the correct values
    // until the funclet completes, at which point the values will be copied back to the ExInfo's REGDISPLAY).

#ifdef _TARGET_ARM_
    m_RegDisplay.pR4  = thisFuncletPtrs.pR4 ;
    m_RegDisplay.pR5  = thisFuncletPtrs.pR5 ;
    m_RegDisplay.pR6  = thisFuncletPtrs.pR6 ;
    m_RegDisplay.pR7  = thisFuncletPtrs.pR7 ;
    m_RegDisplay.pR8  = thisFuncletPtrs.pR8 ;
    m_RegDisplay.pR9  = thisFuncletPtrs.pR9 ;
    m_RegDisplay.pR10 = thisFuncletPtrs.pR10;
    m_RegDisplay.pR11 = thisFuncletPtrs.pR11;

#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");

#else
    // Save the preserved regs portion of the REGDISPLAY across the unwind through the C# EH dispatch code.
    m_RegDisplay.pRbp = thisFuncletPtrs.pRbp;
    m_RegDisplay.pRdi = thisFuncletPtrs.pRdi;
    m_RegDisplay.pRsi = thisFuncletPtrs.pRsi;
    m_RegDisplay.pRbx = thisFuncletPtrs.pRbx;
#ifdef _TARGET_AMD64_
    m_RegDisplay.pR12 = thisFuncletPtrs.pR12;
    m_RegDisplay.pR13 = thisFuncletPtrs.pR13;
    m_RegDisplay.pR14 = thisFuncletPtrs.pR14;
    m_RegDisplay.pR15 = thisFuncletPtrs.pR15;
#endif // _TARGET_AMD64_
#endif // _TARGET_ARM_
}


// The invoke of a funclet is a bit special and requires an assembly thunk, but we don't want to break the
// stackwalk due to this.  So this routine will unwind through the assembly thunks used to invoke funclets.
// It's also used to disambiguate exceptionally- and non-exceptionally-invoked funclets.
void StackFrameIterator::UnwindFuncletInvokeThunk()
{
    ASSERT((m_dwFlags & MethodStateCalculated) == 0);

#if defined(CORERT) // @TODO: CORERT: Currently no funclet invoke defined in a portable way
    return;
#else // defined(CORERT)
    ASSERT(CategorizeUnadjustedReturnAddress(m_ControlPC) == InFuncletInvokeThunk);

    PTR_UIntNative SP;

#ifdef _TARGET_X86_
    // First, unwind RhpCallFunclet
    SP = (PTR_UIntNative)(m_RegDisplay.SP + 0x4);   // skip the saved assembly-routine-EBP
    m_RegDisplay.SetAddrOfIP(SP);
    m_RegDisplay.SetIP(*SP++);
    m_RegDisplay.SetSP((UIntNative)dac_cast<TADDR>(SP));
    m_ControlPC = dac_cast<PTR_VOID>(*(m_RegDisplay.pIP));

    ASSERT(
        EQUALS_CODE_ADDRESS(m_ControlPC, RhpCallCatchFunclet2) ||
        EQUALS_CODE_ADDRESS(m_ControlPC, RhpCallFinallyFunclet2) ||
        EQUALS_CODE_ADDRESS(m_ControlPC, RhpCallFilterFunclet2)
        );
#endif

    bool isFilterInvoke = EQUALS_CODE_ADDRESS(m_ControlPC, RhpCallFilterFunclet2);

#ifdef _TARGET_AMD64_
    if (isFilterInvoke)
    {
        SP = (PTR_UIntNative)(m_RegDisplay.SP + 0x20);
        m_RegDisplay.pRbp = SP++;
    }
    else
    {
        // Save the preserved regs portion of the REGDISPLAY across the unwind through the C# EH dispatch code.
        m_funcletPtrs.pRbp = m_RegDisplay.pRbp;
        m_funcletPtrs.pRdi = m_RegDisplay.pRdi;
        m_funcletPtrs.pRsi = m_RegDisplay.pRsi;
        m_funcletPtrs.pRbx = m_RegDisplay.pRbx;
        m_funcletPtrs.pR12 = m_RegDisplay.pR12;
        m_funcletPtrs.pR13 = m_RegDisplay.pR13;
        m_funcletPtrs.pR14 = m_RegDisplay.pR14;
        m_funcletPtrs.pR15 = m_RegDisplay.pR15;

        if (EQUALS_CODE_ADDRESS(m_ControlPC, RhpCallCatchFunclet2))
        {
            SP = (PTR_UIntNative)(m_RegDisplay.SP + 0x38);
        }
        else
        {
            SP = (PTR_UIntNative)(m_RegDisplay.SP + 0x28);
        }

        m_RegDisplay.pRbp = SP++;
        m_RegDisplay.pRdi = SP++;
        m_RegDisplay.pRsi = SP++;
        m_RegDisplay.pRbx = SP++;
        m_RegDisplay.pR12 = SP++;
        m_RegDisplay.pR13 = SP++;
        m_RegDisplay.pR14 = SP++;
        m_RegDisplay.pR15 = SP++;
    }
#elif defined(_TARGET_X86_)
    if (isFilterInvoke)
    {
        SP = (PTR_UIntNative)(m_RegDisplay.SP + 0x4);
        m_RegDisplay.pRbp = SP++;
    }
    else
    {
        // Save the preserved regs portion of the REGDISPLAY across the unwind through the C# EH dispatch code.
        m_funcletPtrs.pRbp = m_RegDisplay.pRbp;
        m_funcletPtrs.pRdi = m_RegDisplay.pRdi;
        m_funcletPtrs.pRsi = m_RegDisplay.pRsi;
        m_funcletPtrs.pRbx = m_RegDisplay.pRbx;

        SP = (PTR_UIntNative)(m_RegDisplay.SP + 0x4);

        m_RegDisplay.pRdi = SP++;
        m_RegDisplay.pRsi = SP++;
        m_RegDisplay.pRbx = SP++;
        m_RegDisplay.pRbp = SP++;
    }
#elif defined(_TARGET_ARM_)
    if (isFilterInvoke)
    {
        SP = (PTR_UIntNative)(m_RegDisplay.SP + 0x4);
        m_RegDisplay.pR7 = SP++;
        m_RegDisplay.pR11 = SP++;
    }
    else
    {
        // RhpCallCatchFunclet puts a couple of extra things on the stack that aren't put there by the other two
        // thunks, but we don't need to know what they are here, so we just skip them.
        UIntNative uOffsetToR4 = EQUALS_CODE_ADDRESS(m_ControlPC, RhpCallCatchFunclet2) ? 0xC : 0x4;

        // Save the preserved regs portion of the REGDISPLAY across the unwind through the C# EH dispatch code.
        m_funcletPtrs.pR4  = m_RegDisplay.pR4;
        m_funcletPtrs.pR5  = m_RegDisplay.pR5;
        m_funcletPtrs.pR6  = m_RegDisplay.pR6;
        m_funcletPtrs.pR7  = m_RegDisplay.pR7;
        m_funcletPtrs.pR8  = m_RegDisplay.pR8;
        m_funcletPtrs.pR9  = m_RegDisplay.pR9;
        m_funcletPtrs.pR10 = m_RegDisplay.pR10;
        m_funcletPtrs.pR11 = m_RegDisplay.pR11;

        SP = (PTR_UIntNative)(m_RegDisplay.SP + uOffsetToR4);

        m_RegDisplay.pR4  = SP++;
        m_RegDisplay.pR5  = SP++;
        m_RegDisplay.pR6  = SP++;
        m_RegDisplay.pR7  = SP++;
        m_RegDisplay.pR8  = SP++;
        m_RegDisplay.pR9  = SP++;
        m_RegDisplay.pR10 = SP++;
        m_RegDisplay.pR11 = SP++;
    }

#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");

#else
    SP = (PTR_UIntNative)(m_RegDisplay.SP);
    ASSERT_UNCONDITIONALLY("NYI for this arch");
#endif
    m_RegDisplay.SetAddrOfIP((PTR_PCODE)SP);
    m_RegDisplay.SetIP(*SP++);
    m_RegDisplay.SetSP((UIntNative)dac_cast<TADDR>(SP));
    m_ControlPC = dac_cast<PTR_VOID>(*(m_RegDisplay.pIP));

    // We expect to be called by the runtime's C# EH implementation, and since this function's notion of how 
    // to unwind through the stub is brittle relative to the stub itself, we want to check as soon as we can.
    ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC) && "unwind from funclet invoke stub failed");
#endif // defined(CORERT)
}

// For a given target architecture, the layout of this structure must precisely match the
// stack frame layout used by the associated architecture-specific RhpUniversalTransition
// implementation.
struct UniversalTransitionStackFrame
{

// In DAC builds, the "this" pointer refers to an object in the DAC host.
#define GET_POINTER_TO_FIELD(_FieldName) \
    (PTR_UIntNative)PTR_HOST_MEMBER(UniversalTransitionStackFrame, this, _FieldName)

#ifdef _TARGET_AMD64_

    // Conservative GC reporting must be applied to everything between the base of the
    // ReturnBlock and the top of the StackPassedArgs.
private:
    UIntNative m_calleeArgumentHomes[4];    // ChildSP+000 CallerSP-080 (0x20 bytes)
    Fp128 m_fpArgRegs[4];                   // ChildSP+020 CallerSP-060 (0x40 bytes)    (xmm0-xmm3)
    UIntNative m_returnBlock[2];            // ChildSP+060 CallerSP-020 (0x10 bytes)
    UIntNative m_alignmentPad;              // ChildSP+070 CallerSP-010 (0x8 bytes)
    UIntNative m_callerRetaddr;             // ChildSP+078 CallerSP-008 (0x8 bytes)
    UIntNative m_intArgRegs[4];             // ChildSP+080 CallerSP+000 (0x20 bytes)    (rcx,rdx,r8,r9)
    UIntNative m_stackPassedArgs[1];        // ChildSP+0a0 CallerSP+020 (unknown size)

public:
    PTR_UIntNative get_CallerSP() { return GET_POINTER_TO_FIELD(m_intArgRegs[0]); }
    PTR_UIntNative get_AddressOfPushedCallerIP() { return GET_POINTER_TO_FIELD(m_callerRetaddr); }
    PTR_UIntNative get_LowerBoundForConservativeReporting() { return GET_POINTER_TO_FIELD(m_returnBlock[0]); }

    void UnwindNonVolatileRegisters(REGDISPLAY * pRegisterSet)
    {
        // RhpUniversalTransition does not touch any non-volatile state on amd64.
        UNREFERENCED_PARAMETER(pRegisterSet);
    }

#elif defined(_TARGET_ARM_)

    // Conservative GC reporting must be applied to everything between the base of the
    // ReturnBlock and the top of the StackPassedArgs.
private:
    UIntNative m_pushedR11;                 // ChildSP+000 CallerSP-078 (0x4 bytes)     (r11)
    UIntNative m_pushedLR;                  // ChildSP+004 CallerSP-074 (0x4 bytes)     (lr)
    UInt64 m_fpArgRegs[8];                  // ChildSP+008 CallerSP-070 (0x40 bytes)    (d0-d7)
    UInt64 m_returnBlock[4];                // ChildSP+048 CallerSP-030 (0x20 bytes)
    UIntNative m_intArgRegs[4];             // ChildSP+068 CallerSP-010 (0x10 bytes)    (r0-r3)
    UIntNative m_stackPassedArgs[1];        // ChildSP+078 CallerSP+000 (unknown size)

public:
    PTR_UIntNative get_CallerSP() { return GET_POINTER_TO_FIELD(m_stackPassedArgs[0]); }
    PTR_UIntNative get_AddressOfPushedCallerIP() { return GET_POINTER_TO_FIELD(m_pushedLR); }
    PTR_UIntNative get_LowerBoundForConservativeReporting() { return GET_POINTER_TO_FIELD(m_returnBlock[0]); }

    void UnwindNonVolatileRegisters(REGDISPLAY * pRegisterSet)
    {
        pRegisterSet->pR11 = GET_POINTER_TO_FIELD(m_pushedR11);
    }

#elif defined(_TARGET_X86_)

    // Conservative GC reporting must be applied to everything between the base of the
    // IntArgRegs and the top of the StackPassedArgs.
private:
    UIntNative m_intArgRegs[2];             // ChildSP+000 CallerSP-018 (0x8 bytes)     (edx,ecx)
    UIntNative m_returnBlock[2];            // ChildSP+008 CallerSP-010 (0x8 bytes)
    UIntNative m_pushedEBP;                 // ChildSP+010 CallerSP-008 (0x4 bytes)
    UIntNative m_callerRetaddr;             // ChildSP+014 CallerSP-004 (0x4 bytes)
    UIntNative m_stackPassedArgs[1];        // ChildSP+018 CallerSP+000 (unknown size)

public:
    PTR_UIntNative get_CallerSP() { return GET_POINTER_TO_FIELD(m_stackPassedArgs[0]); }
    PTR_UIntNative get_AddressOfPushedCallerIP() { return GET_POINTER_TO_FIELD(m_callerRetaddr); }
    PTR_UIntNative get_LowerBoundForConservativeReporting() { return GET_POINTER_TO_FIELD(m_intArgRegs[0]); }

    void UnwindNonVolatileRegisters(REGDISPLAY * pRegisterSet)
    {
        pRegisterSet->pRbp = GET_POINTER_TO_FIELD(m_pushedEBP);
    }

#else
#error NYI for this arch
#endif

#undef GET_POINTER_TO_FIELD

};

typedef DPTR(UniversalTransitionStackFrame) PTR_UniversalTransitionStackFrame;

// NOTE: This function always publishes a non-NULL conservative stack range lower bound.
//
// NOTE: In x86 cases, the unwound callsite often uses a calling convention that expects some amount
// of stack-passed argument space to be callee-popped before control returns (or unwinds) to the
// callsite.  Since the callsite signature (and thus the amount of callee-popped space) is unknown,
// the recovered SP does not account for the callee-popped space is therefore "wrong" for the
// purposes of unwind.  This implies that any x86 function which calls into RhpUniversalTransition
// must have a frame pointer to ensure that the incorrect SP value is ignored and does not break the
// unwind.
void StackFrameIterator::UnwindUniversalTransitionThunk()
{
    ASSERT((m_dwFlags & MethodStateCalculated) == 0);

#if defined(CORERT) // @TODO: CORERT: Corresponding helper code is only defined in assembly code
    return;
#else // defined(CORERT)
    ASSERT(CategorizeUnadjustedReturnAddress(m_ControlPC) == InUniversalTransitionThunk);

    // The current PC is within RhpUniversalTransition, so establish a view of the surrounding stack frame.
    // NOTE: In DAC builds, the pointer will refer to a newly constructed object in the DAC host.
    UniversalTransitionStackFrame * stackFrame = (PTR_UniversalTransitionStackFrame)m_RegDisplay.SP;

    stackFrame->UnwindNonVolatileRegisters(&m_RegDisplay);

    PTR_UIntNative addressOfPushedCallerIP = stackFrame->get_AddressOfPushedCallerIP();
    m_RegDisplay.SetAddrOfIP((PTR_PCODE)addressOfPushedCallerIP);
    m_RegDisplay.SetIP(*addressOfPushedCallerIP);
    m_RegDisplay.SetSP((UIntNative)dac_cast<TADDR>(stackFrame->get_CallerSP()));
    m_ControlPC = dac_cast<PTR_VOID>(*(m_RegDisplay.pIP));

    // All universal transition cases rely on conservative GC reporting being applied to the
    // full argument set that flowed into the call.  Report the lower bound of this range (the
    // caller will compute the upper bound).
    PTR_UIntNative pLowerBound = stackFrame->get_LowerBoundForConservativeReporting();
    ASSERT(pLowerBound != NULL);
    ASSERT(m_pConservativeStackRangeLowerBound == NULL);
    m_pConservativeStackRangeLowerBound = pLowerBound;
#endif // defined(CORERT)
}

#ifdef _TARGET_AMD64_
#define STACK_ALIGN_SIZE 16
#elif defined(_TARGET_ARM_)
#define STACK_ALIGN_SIZE 8
#elif defined(_TARGET_ARM64_)
#define STACK_ALIGN_SIZE 16
#elif defined(_TARGET_X86_)
#define STACK_ALIGN_SIZE 4
#endif

#ifdef _TARGET_AMD64_
struct CALL_DESCR_CONTEXT
{
    UIntNative  Rbp;
    UIntNative  Rsi;
    UIntNative  Rbx;
    UIntNative  IP;
};
#elif defined(_TARGET_ARM_)
struct CALL_DESCR_CONTEXT
{
    UIntNative  R4;
    UIntNative  R5;
    UIntNative  R7;
    UIntNative  IP;
};
#elif defined(_TARGET_ARM64_)
// @TODO: Add ARM64 entries
struct CALL_DESCR_CONTEXT
{
    UIntNative IP;
};
#elif defined(_TARGET_X86_)
struct CALL_DESCR_CONTEXT
{
    UIntNative  Rbx;
    UIntNative  Rbp;
    UIntNative  IP;
};
#else
#error NYI - For this arch
#endif

typedef DPTR(CALL_DESCR_CONTEXT) PTR_CALL_DESCR_CONTEXT;

void StackFrameIterator::UnwindCallDescrThunk()
{
    ASSERT((m_dwFlags & MethodStateCalculated) == 0);

#if defined(CORERT) // @TODO: CORERT: Corresponding helper code is only defined in assembly code
    return;
#else // defined(CORERT)
    ASSERT(CategorizeUnadjustedReturnAddress(m_ControlPC) == InCallDescrThunk);

    UIntNative newSP;
#ifdef _TARGET_AMD64_
    // RBP points to the SP that we want to capture. (This arrangement allows for
    // the arguments from this function to be loaded into memory with an adjustment
    // to SP, like an alloca
    newSP = *(PTR_UIntNative)m_RegDisplay.pRbp;

    PTR_CALL_DESCR_CONTEXT pContext = (PTR_CALL_DESCR_CONTEXT)newSP;

    m_RegDisplay.pRbp = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, Rbp);
    m_RegDisplay.pRsi = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, Rsi);
    m_RegDisplay.pRbx = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, Rbx);

    // And adjust SP to be the state that it should be in just after returning from
    // the CallDescrFunction
    newSP += sizeof(CALL_DESCR_CONTEXT);
#elif defined(_TARGET_ARM_)
    // R7 points to the SP that we want to capture. (This arrangement allows for
    // the arguments from this function to be loaded into memory with an adjustment
    // to SP, like an alloca
    newSP = *(PTR_UIntNative)m_RegDisplay.pR7;
    PTR_CALL_DESCR_CONTEXT pContext = (PTR_CALL_DESCR_CONTEXT)newSP;

    m_RegDisplay.pR4 = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, R4);
    m_RegDisplay.pR5 = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, R5);
    m_RegDisplay.pR7 = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, R7);

    // And adjust SP to be the state that it should be in just after returning from
    // the CallDescrFunction
    newSP += sizeof(CALL_DESCR_CONTEXT);

#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");

#elif defined(_TARGET_X86_)
    // RBP points to the SP that we want to capture. (This arrangement allows for
    // the arguments from this function to be loaded into memory with an adjustment
    // to SP, like an alloca
    newSP = *(PTR_UIntNative)m_RegDisplay.pRbp;

    PTR_CALL_DESCR_CONTEXT pContext = (PTR_CALL_DESCR_CONTEXT)(newSP - offsetof(CALL_DESCR_CONTEXT, Rbp));

    m_RegDisplay.pRbp = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, Rbp);
    m_RegDisplay.pRbx = PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, Rbx);

    // And adjust SP to be the state that it should be in just after returning from
    // the CallDescrFunction
    newSP += sizeof(CALL_DESCR_CONTEXT) - offsetof(CALL_DESCR_CONTEXT, Rbp);
#else
    ASSERT_UNCONDITIONALLY("NYI for this arch");
#endif

    m_RegDisplay.SetAddrOfIP(PTR_TO_MEMBER(CALL_DESCR_CONTEXT, pContext, IP));
    m_RegDisplay.SetIP(pContext->IP);
    m_RegDisplay.SetSP(newSP);
    m_ControlPC = dac_cast<PTR_VOID>(pContext->IP);
#endif // defined(CORERT)
}

void StackFrameIterator::UnwindThrowSiteThunk()
{
    ASSERT((m_dwFlags & MethodStateCalculated) == 0);

#if defined(CORERT) // @TODO: CORERT: no portable version of throw helpers
    return;
#else // defined(CORERT)
    ASSERT(CategorizeUnadjustedReturnAddress(m_ControlPC) == InThrowSiteThunk);

    const UIntNative STACKSIZEOF_ExInfo = ((sizeof(ExInfo) + (STACK_ALIGN_SIZE-1)) & ~(STACK_ALIGN_SIZE-1));
#ifdef _TARGET_AMD64_
    const UIntNative SIZEOF_OutgoingScratch = 0x20;
#else
    const UIntNative SIZEOF_OutgoingScratch = 0;
#endif

    PTR_PAL_LIMITED_CONTEXT pContext = (PTR_PAL_LIMITED_CONTEXT)
                                        (m_RegDisplay.SP + SIZEOF_OutgoingScratch + STACKSIZEOF_ExInfo);

#ifdef _TARGET_AMD64_
    m_RegDisplay.pRbp = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rbp);
    m_RegDisplay.pRdi = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rdi);
    m_RegDisplay.pRsi = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rsi);
    m_RegDisplay.pRbx = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rbx);
    m_RegDisplay.pR12 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R12);
    m_RegDisplay.pR13 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R13);
    m_RegDisplay.pR14 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R14);
    m_RegDisplay.pR15 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R15);
#elif defined(_TARGET_ARM_)
    m_RegDisplay.pR4  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R4);
    m_RegDisplay.pR5  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R5);
    m_RegDisplay.pR6  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R6);
    m_RegDisplay.pR7  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R7);
    m_RegDisplay.pR8  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R8);
    m_RegDisplay.pR9  = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R9);
    m_RegDisplay.pR10 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R10);
    m_RegDisplay.pR11 = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, R11);
#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");
#elif defined(_TARGET_X86_)
    m_RegDisplay.pRbp = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rbp);
    m_RegDisplay.pRdi = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rdi);
    m_RegDisplay.pRsi = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rsi);
    m_RegDisplay.pRbx = PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, Rbx);
#else
    ASSERT_UNCONDITIONALLY("NYI for this arch");
#endif

    m_RegDisplay.SetAddrOfIP(PTR_TO_MEMBER(PAL_LIMITED_CONTEXT, pContext, IP));
    m_RegDisplay.SetIP(pContext->IP);
    m_RegDisplay.SetSP(pContext->GetSp());
    m_ControlPC = dac_cast<PTR_VOID>(pContext->IP);

    // We expect the throw site to be in managed code, and since this function's notion of how to unwind 
    // through the stub is brittle relative to the stub itself, we want to check as soon as we can.
    ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC) && "unwind from throw site stub failed");
#endif // defined(CORERT)
}

// If our control PC indicates that we're in one of the thunks we use to make managed callouts from the
// runtime we need to adjust the frame state to that of the managed method that previously called into the
// runtime (i.e. skip the intervening unmanaged frames).
//
// NOTE: This function always publishes a non-NULL conservative stack range lower bound.
//
// NOTE: In x86 cases, the unwound callsite often uses a calling convention that expects some amount
// of stack-passed argument space to be callee-popped before control returns (or unwinds) to the
// callsite.  Since the callsite signature (and thus the amount of callee-popped space) is unknown,
// the recovered SP does not account for the callee-popped space is therefore "wrong" for the
// purposes of unwind.  This implies that any x86 function which might trigger a managed callout
// (e.g., any function which triggers interface dispatch) must have a frame pointer to ensure that
// the incorrect SP value is ignored and does not break the unwind.
void StackFrameIterator::UnwindManagedCalloutThunk()
{
#if defined(CORERT) // @TODO: CORERT: no portable version of managed callout defined
    return;
#else // defined(CORERT)
    ASSERT(CategorizeUnadjustedReturnAddress(m_ControlPC) == InManagedCalloutThunk);

    // We're in a special thunk we use to call into managed code from unmanaged code in the runtime. This
    // thunk sets up an FP frame with a pointer to a PInvokeTransitionFrame erected by the managed method
    // which called into the runtime in the first place (actually a stub called by that managed method).
    // Thus we can unwind from one managed method to the previous one, skipping all the unmanaged frames
    // in the middle.
    //
    // On all architectures this transition frame pointer is pushed at a well-known offset from FP.
    PTR_VOID pEntryToRuntimeFrame = *(PTR_PTR_VOID)(m_RegDisplay.GetFP() +
                                                 MANAGED_CALLOUT_THUNK_TRANSITION_FRAME_POINTER_OFFSET);

    // Reload the iterator with the state saved into the PInvokeTransitionFrame.
    // NOTE: EH stack walks cannot reach this point (since the caller generates a failfast in this case).
    // NOTE: This call may locate a nested conservative range if the original callsite into the runtime
    // resides in an assembly thunk and not in normal managed code.  In this case InternalInit will
    // unwind through the thunk and back to the nearest managed frame, and therefore may see a
    // conservative range reported by one of the thunks encountered during this "nested" unwind.
    ASSERT(!(m_dwFlags & ApplyReturnAddressAdjustment));
    ASSERT(m_pConservativeStackRangeLowerBound == NULL);
    InternalInit(m_pThread, GetPInvokeTransitionFrame(pEntryToRuntimeFrame), GcStackWalkFlags);
    ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC));
    PTR_UIntNative pNestedLowerBound = m_pConservativeStackRangeLowerBound;

    // Additionally the initial managed method (the one that called into the runtime) may have pushed some
    // arguments containing GC references on the stack. Since the managed callout initiated by the runtime
    // has an unrelated signature, there's nobody reporting any of these references to the GC. To avoid
    // having to store signature information for what might be potentially a lot of methods (we use this
    // mechanism for certain edge cases in interface invoke) we conservatively report a range of the stack
    // that might contain GC references. Such references will be in either the outgoing stack argument
    // slots of the calling method or in argument registers spilled to the stack in the prolog of the stub
    // they use to call into the runtime.
    //
    // The lower bound of this range we define as the transition frame itself. We just computed this
    // address and it's guaranteed to be lower than (but quite close to) that of any spilled argument
    // register (see comments in the various versions of RhpInterfaceDispatchSlow).
    PTR_UIntNative pLowerBound = (PTR_UIntNative)pEntryToRuntimeFrame;
    FAILFAST_OR_DAC_FAIL((pNestedLowerBound == NULL) || (pNestedLowerBound > pLowerBound));
    m_pConservativeStackRangeLowerBound = pLowerBound;
#endif // defined(CORERT)
}

bool StackFrameIterator::IsValid()
{
    return (m_ControlPC != 0);
}

void StackFrameIterator::Next()
{
    NextInternal();
    STRESS_LOG1(LF_STACKWALK, LL_INFO10000, "   %p\n", m_ControlPC);
}

void StackFrameIterator::NextInternal()
{
UnwindOutOfCurrentManagedFrame:
    ASSERT(m_dwFlags & MethodStateCalculated);
    m_dwFlags &= ~(ExCollide|MethodStateCalculated|UnwoundReversePInvoke);
    ASSERT(IsValid());

    m_pHijackedReturnValue = NULL;
    m_HijackedReturnValueKind = GCRK_Unknown;

#ifdef _DEBUG
    m_ControlPC = dac_cast<PTR_VOID>((void*)666);
#endif // _DEBUG

    // Clear any preceding published conservative range.  The current unwind will compute a new range
    // from scratch if one is needed.
    m_pConservativeStackRangeLowerBound = NULL;
    m_pConservativeStackRangeUpperBound = NULL;

#if defined(_DEBUG) && !defined(DACCESS_COMPILE)
    UIntNative DEBUG_preUnwindSP = m_RegDisplay.GetSP();
#endif

    PTR_VOID pPreviousTransitionFrame;
    FAILFAST_OR_DAC_FAIL(GetCodeManager()->UnwindStackFrame(&m_methodInfo, m_codeOffset, &m_RegDisplay, &pPreviousTransitionFrame));
    bool doingFuncletUnwind = GetCodeManager()->IsFunclet(&m_methodInfo);

    if (pPreviousTransitionFrame != NULL)
    {
        ASSERT(!doingFuncletUnwind);

        if (pPreviousTransitionFrame == TOP_OF_STACK_MARKER)
        {
            m_ControlPC = 0;
        }
        else
        {
            // NOTE: If this is an EH stack walk, then reinitializing the iterator using the GC stack
            // walk flags is incorrect.  That said, this is OK because the exception dispatcher will
            // immediately trigger a failfast when it sees the UnwoundReversePInvoke flag.
            // NOTE: This can generate a conservative stack range if the recovered PInvoke callsite
            // resides in an assembly thunk and not in normal managed code.  In this case InternalInit
            // will unwind through the thunk and back to the nearest managed frame, and therefore may
            // see a conservative range reported by one of the thunks encountered during this "nested"
            // unwind.
            InternalInit(m_pThread, GetPInvokeTransitionFrame(pPreviousTransitionFrame), GcStackWalkFlags);
            ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC));
        }
        m_dwFlags |= UnwoundReversePInvoke;
    }
    else
    {
        // if the thread is safe to walk, it better not have a hijack in place.
        ASSERT((ThreadStore::GetCurrentThread() == m_pThread) || !m_pThread->DangerousCrossThreadIsHijacked());

        m_ControlPC = dac_cast<PTR_VOID>(*(m_RegDisplay.GetAddrOfIP()));

        PTR_VOID collapsingTargetFrame = NULL;

        // Starting from the unwound return address, unwind further (if needed) until reaching
        // either the next managed frame (i.e., the next frame that should be yielded from the
        // stack frame iterator) or a collision point that requires complex handling.

        bool exCollide = false;
        ReturnAddressCategory category = CategorizeUnadjustedReturnAddress(m_ControlPC);

        if (doingFuncletUnwind)
        {
            ASSERT(m_pendingFuncletFramePointer == NULL);
            ASSERT(m_FramePointer != NULL);

            if (category == InFuncletInvokeThunk)
            {
                // The iterator is unwinding out of an exceptionally invoked funclet.  Before proceeding,
                // record the funclet frame pointer so that the iterator can verify that the remainder of
                // the stack walk encounters "owner frames" (i.e., parent funclets or the main code body)
                // in the expected order.
                // NOTE: m_pendingFuncletFramePointer will be cleared by HandleExCollide the stack walk
                // collides with the ExInfo that invoked this funclet.
                m_pendingFuncletFramePointer = m_FramePointer;

                // Unwind through the funclet invoke assembly thunk to reach the topmost managed frame in
                // the exception dispatch code.  All non-GC stack walks collide at this point (whereas GC
                // stack walks collide at the throw site which is reached after processing all of the
                // exception dispatch frames).
                UnwindFuncletInvokeThunk();
                if (!(m_dwFlags & CollapseFunclets))
                {
                    exCollide = true;
                }
            }
            else if (category == InManagedCode)
            {
                // Non-exceptionally invoked funclet case.  The caller is processed as a normal managed
                // frame, with the caveat that funclet collapsing must be applied in GC stack walks (since
                // the caller is either a parent funclet or the main code body and the leafmost funclet
                // already provided GC information for the entire function).
                if (m_dwFlags & CollapseFunclets)
                {
                    collapsingTargetFrame = m_FramePointer;
                }
            }
            else
            {
                FAILFAST_OR_DAC_FAIL_UNCONDITIONALLY("Unexpected thunk encountered when unwinding out of a funclet.");
            }
        }
        else if (category != InManagedCode)
        {
            // Unwinding the current (non-funclet) managed frame revealed that its caller is one of the
            // well-known assembly thunks.  Unwind through the thunk to find the next managed frame
            // that should be yielded from the stack frame iterator.
            // NOTE: It is generally possible for a sequence of multiple thunks to appear "on top of
            // each other" on the stack (e.g., the CallDescrThunk can be used to invoke the
            // UniversalTransitionThunk), but EH thunks can never appear in such sequences.

            if (IsNonEHThunk(category))
            {
                // Unwind the current sequence of one or more thunks until the next managed frame is reached.
                // NOTE: This can generate a conservative stack range if one or more of the thunks in the
                // sequence report a conservative lower bound.
                UnwindNonEHThunkSequence();
            }
            else if (category == InThrowSiteThunk)
            {
                // EH stack walks collide at the funclet invoke thunk and are never expected to encounter
                // throw sites (except in illegal cases such as exceptions escaping from the managed
                // exception dispatch code itself).
                FAILFAST_OR_DAC_FAIL_MSG(!(m_dwFlags & ApplyReturnAddressAdjustment),
                    "EH stack walk is attempting to propagate an exception across a throw site.");

                UnwindThrowSiteThunk();

                if (m_dwFlags & CollapseFunclets)
                {
                    UIntNative postUnwindSP = m_RegDisplay.SP;

                    if (m_pNextExInfo && (postUnwindSP > ((UIntNative)dac_cast<TADDR>(m_pNextExInfo))))
                    {
                        // This GC stack walk has processed all managed exception frames associated with the
                        // current throw site, meaning it has now collided with the associated ExInfo.
                        exCollide = true;
                    }
                }
            }
            else
            {
                FAILFAST_OR_DAC_FAIL_UNCONDITIONALLY("Unexpected thunk encountered when unwinding out of a non-funclet.");
            }
        }

        if (exCollide)
        {
            // OK, so we just hit (collided with) an exception throw point.  We continue by consulting the 
            // ExInfo.

            // In the GC stackwalk, this means walking all the way off the end of the managed exception
            // dispatch code to the throw site.  In the EH stackwalk, this means hitting the special funclet
            // invoke ASM thunks.

            // Double-check that the ExInfo that is being consulted is at or below the 'current' stack pointer
            ASSERT(DEBUG_preUnwindSP <= (UIntNative)m_pNextExInfo);

            ASSERT(collapsingTargetFrame == NULL);

            collapsingTargetFrame = HandleExCollide(m_pNextExInfo);
        }

        // Now that all assembly thunks and ExInfo collisions have been processed, it is guaranteed
        // that the next managed frame has been located.  The located frame must now be yielded
        // from the iterator with the one and only exception being cases where a managed frame must
        // be skipped due to funclet collapsing.

        ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC));

        if (collapsingTargetFrame != NULL)
        {
            // The iterator is positioned on a parent funclet or main code body in a function where GC
            // information has already been reported by the leafmost funclet, implying that the current
            // frame needs to be skipped by the GC stack walk.  In general, the GC stack walk must skip
            // all parent frames that are "part of the same function" (i.e., have the same frame
            // pointer).
            ASSERT(m_dwFlags & CollapseFunclets);
            CalculateCurrentMethodState();
            ASSERT(IsValid());
            FAILFAST_OR_DAC_FAIL(m_FramePointer == collapsingTargetFrame);

            // Fail if the skipped frame has no associated conservative stack range (since any
            // attached stack range is about to be dropped without ever being reported to the GC).
            // This should never happen since funclet collapsing cases and only triggered when
            // unwinding out of managed frames and never when unwinding out of the thunks that report
            // conservative ranges.
            FAILFAST_OR_DAC_FAIL(m_pConservativeStackRangeLowerBound == NULL);

            STRESS_LOG0(LF_STACKWALK, LL_INFO10000, "[ KeepUnwinding ]\n");
            goto UnwindOutOfCurrentManagedFrame;
        }

        // Before yielding this frame, indicate that it was located via an ExInfo collision as
        // opposed to normal unwind.
        if (exCollide)
            m_dwFlags |= ExCollide;
    }

    // At this point, the iterator is in an invalid state if there are no more managed frames
    // on the current stack, and is otherwise positioned on the next managed frame to yield to
    // the caller.
    PrepareToYieldFrame();
}

// NOTE: This function will publish a non-NULL conservative stack range lower bound if and
// only if one or more of the thunks in the sequence report conservative stack ranges.
void StackFrameIterator::UnwindNonEHThunkSequence()
{
    ReturnAddressCategory category = CategorizeUnadjustedReturnAddress(m_ControlPC);
    ASSERT(IsNonEHThunk(category));

    // Unwind the current sequence of thunks until the next managed frame is reached, being
    // careful to detect and aggregate any conservative stack ranges reported by the thunks.
    PTR_UIntNative pLowestLowerBound = NULL;
    PTR_UIntNative pPrecedingLowerBound = NULL;
    while (category != InManagedCode)
    {
        ASSERT(m_pConservativeStackRangeLowerBound == NULL);

        if (category == InCallDescrThunk)
        {
            UnwindCallDescrThunk();
        }
        else if (category == InUniversalTransitionThunk)
        {
            UnwindUniversalTransitionThunk();
            ASSERT(m_pConservativeStackRangeLowerBound != NULL);
        }
        else if (category == InManagedCalloutThunk)
        {
            // Exception propagation across managed callouts is illegal (i.e., it violates the
            // fundamental contract that all managed callout implementations must honor).
            FAILFAST_OR_DAC_FAIL_MSG(!(m_dwFlags & ApplyReturnAddressAdjustment),
                "EH stack walk is attempting to propagate an exception across a managed callout.");

            UnwindManagedCalloutThunk();
            ASSERT(m_pConservativeStackRangeLowerBound != NULL);
        }
        else
        {
            FAILFAST_OR_DAC_FAIL_UNCONDITIONALLY("Unexpected thunk encountered when unwinding a non-EH thunk sequence.");
        }

        if (m_pConservativeStackRangeLowerBound != NULL)
        {
            // The newly unwound thunk reported a conservative stack range lower bound.  The thunk
            // sequence being unwound needs to generate a single conservative range that will be
            // reported along with the managed frame eventually yielded by the iterator.  To ensure
            // sufficient reporting, this range always extends from the first (i.e., lowest) lower
            // bound all the way to the top of the outgoing arguments area in the next managed frame.
            // This aggregate range therefore covers all intervening thunk frames (if any), and also
            // covers all necessary conservative ranges in the pathological case where a sequence of
            // thunks contains multiple frames which report distinct conservative lower bound values.
            //
            // Capture the initial lower bound, and assert that the lower bound values are compatible
            // with the "aggregate range" approach described above (i.e., that they never exceed the
            // unwound thunk's stack frame and are always larger than all previously encountered lower
            // bound values).

            if (pLowestLowerBound == NULL)
                pLowestLowerBound = m_pConservativeStackRangeLowerBound;

            FAILFAST_OR_DAC_FAIL(m_pConservativeStackRangeLowerBound < (PTR_UIntNative)m_RegDisplay.SP);
            FAILFAST_OR_DAC_FAIL(m_pConservativeStackRangeLowerBound > pPrecedingLowerBound);
            pPrecedingLowerBound = m_pConservativeStackRangeLowerBound;
            m_pConservativeStackRangeLowerBound = NULL;
        }

        category = CategorizeUnadjustedReturnAddress(m_ControlPC);
    }

    // The iterator has reached the next managed frame.  Publish the computed lower bound value.
    ASSERT(m_pConservativeStackRangeLowerBound == NULL);
    m_pConservativeStackRangeLowerBound = pLowestLowerBound;
}

// This function is called immediately before a given frame is yielded from the iterator
// (i.e., before a given frame is exposed outside of the iterator).  At yield points,
// iterator must either be invalid (indicating that all managed frames have been processed)
// or must describe a valid managed frame.  In the latter case, some common postprocessing
// steps must always be applied before the frame is exposed outside of the iterator.
void StackFrameIterator::PrepareToYieldFrame()
{
    if (!IsValid())
        return;

    ASSERT(m_pInstance->FindCodeManagerByAddress(m_ControlPC));

    if (m_dwFlags & ApplyReturnAddressAdjustment)
        m_ControlPC = AdjustReturnAddressBackward(m_ControlPC);

    // Each time a managed frame is yielded, configure the iterator to explicitly indicate
    // whether or not unwinding to the current frame has revealed a stack range that must be
    // conservatively reported by the GC.
    if ((m_pConservativeStackRangeLowerBound != NULL) && (m_dwFlags & CollapseFunclets))
    {
        // Conservatively reported stack ranges always correspond to the full extent of the
        // argument set (including stack-passed arguments and spilled argument registers) that
        // flowed into a managed callsite which called into the runtime.  The runtime has no
        // knowledge of the callsite signature in these cases, and unwind through these callsites
        // is only possible via the associated assembly thunk (e.g., the ManagedCalloutThunk or
        // UniversalTransitionThunk).
        //
        // The iterator is currently positioned on the managed frame which contains the callsite of
        // interest.  The lower bound of the argument set was already computed while unwinding
        // through the assembly thunk.  The upper bound of the argument set is always at or below
        // the top of the outgoing arguments area in the current managed frame (i.e., in the
        // managed frame which contains the callsite).
        //
        // Compute a conservative upper bound and then publish the total range so that it can be
        // observed by the current GC stack walk (via HasStackRangeToReportConservatively).  Note
        // that the upper bound computation never mutates m_RegDisplay.
        CalculateCurrentMethodState();
        ASSERT(IsValid());
        UIntNative rawUpperBound = GetCodeManager()->GetConservativeUpperBoundForOutgoingArgs(&m_methodInfo, &m_RegDisplay);
        m_pConservativeStackRangeUpperBound = (PTR_UIntNative)rawUpperBound;

        ASSERT(m_pConservativeStackRangeLowerBound != NULL);
        ASSERT(m_pConservativeStackRangeUpperBound != NULL);
        ASSERT(m_pConservativeStackRangeUpperBound > m_pConservativeStackRangeLowerBound);
    }
    else
    {
        m_pConservativeStackRangeLowerBound = NULL;
        m_pConservativeStackRangeUpperBound = NULL;
    }
}

REGDISPLAY * StackFrameIterator::GetRegisterSet()
{
    ASSERT(IsValid());
    return &m_RegDisplay;
}

UInt32 StackFrameIterator::GetCodeOffset()
{
    ASSERT(IsValid());
    return m_codeOffset;
}

ICodeManager * StackFrameIterator::GetCodeManager()
{
    ASSERT(IsValid());
    return m_pCodeManager;
}

MethodInfo * StackFrameIterator::GetMethodInfo()
{
    ASSERT(IsValid());
    return &m_methodInfo;
}

#ifdef DACCESS_COMPILE
#define FAILFAST_OR_DAC_RETURN_FALSE(x) if(!(x)) return false;
#else
#define FAILFAST_OR_DAC_RETURN_FALSE(x) if(!(x)) { ASSERT_UNCONDITIONALLY(#x); RhFailFast(); }
#endif

void StackFrameIterator::CalculateCurrentMethodState()
{
    if (m_dwFlags & MethodStateCalculated)
        return;

    // Assume that the caller is likely to be in the same module
    if (m_pCodeManager == NULL || !m_pCodeManager->FindMethodInfo(m_ControlPC, &m_methodInfo, &m_codeOffset))
    {
        m_pCodeManager = m_pInstance->FindCodeManagerByAddress(m_ControlPC);
        FAILFAST_OR_DAC_FAIL(m_pCodeManager);

        FAILFAST_OR_DAC_FAIL(m_pCodeManager->FindMethodInfo(m_ControlPC, &m_methodInfo, &m_codeOffset));
    }

    m_FramePointer = GetCodeManager()->GetFramePointer(&m_methodInfo, &m_RegDisplay);

    m_dwFlags |= MethodStateCalculated;
}

bool StackFrameIterator::GetHijackedReturnValueLocation(PTR_RtuObjectRef * pLocation, GCRefKind * pKind)
{
    if (GCRK_Unknown == m_HijackedReturnValueKind)
        return false;

    ASSERT((GCRK_Object == m_HijackedReturnValueKind) || (GCRK_Byref == m_HijackedReturnValueKind));

    *pLocation = m_pHijackedReturnValue;
    *pKind = m_HijackedReturnValueKind;
    return true;
}

bool StackFrameIterator::IsNonEHThunk(ReturnAddressCategory category)
{
    switch (category)
    {
        default:
            return false;
        case InManagedCalloutThunk:
        case InUniversalTransitionThunk:
        case InCallDescrThunk:
            return true;
    }
}

bool StackFrameIterator::IsValidReturnAddress(PTR_VOID pvAddress)
{
#if !defined(CORERT) // @TODO: CORERT: no portable version of these helpers defined
    // These are return addresses into functions that call into managed (non-funclet) code, so we might see
    // them as hijacked return addresses.
    ReturnAddressCategory category = CategorizeUnadjustedReturnAddress(pvAddress);

    // All non-EH thunks call out to normal managed code, implying that return addresses into
    // them can be hijacked.
    if (IsNonEHThunk(category))
        return true;

    // Throw site thunks call out to managed code, but control never returns from the managed
    // callee.  As a result, return addresses into these thunks can be hijacked, but the
    // hijacks will never execute.
    if (category == InThrowSiteThunk)
        return true;
#endif // !defined(CORERT)

    return (NULL != GetRuntimeInstance()->FindCodeManagerByAddress(pvAddress));
}

// Support for conservatively reporting GC references in a stack range. This is used when managed methods with
// an unknown signature potentially including GC references call into the runtime and we need to let a GC
// proceed (typically because we call out into managed code again). Instead of storing signature metadata for
// every possible managed method that might make such a call we identify a small range of the stack that might
// contain outgoing arguments. We then report every pointer that looks like it might refer to the GC heap as a
// fixed interior reference.

bool StackFrameIterator::HasStackRangeToReportConservatively()
{
    // When there's no range to report both the lower and upper bounds will be NULL.
    return IsValid() && (m_pConservativeStackRangeUpperBound != NULL);
}

void StackFrameIterator::GetStackRangeToReportConservatively(PTR_RtuObjectRef * ppLowerBound, PTR_RtuObjectRef * ppUpperBound)
{
    ASSERT(HasStackRangeToReportConservatively());
    *ppLowerBound = (PTR_RtuObjectRef)m_pConservativeStackRangeLowerBound;
    *ppUpperBound = (PTR_RtuObjectRef)m_pConservativeStackRangeUpperBound;
}

// helpers to ApplyReturnAddressAdjustment
// The adjustment is made by EH to ensure that the ControlPC of a callsite stays within the containing try region.
// We adjust by the minimum instruction size on the target-architecture (1-byte on x86 and AMD64, 2-bytes on ARM)
PTR_VOID StackFrameIterator::AdjustReturnAddressForward(PTR_VOID controlPC)
{
#ifdef _TARGET_ARM_
    return (PTR_VOID)(((PTR_UInt8)controlPC) + 2);
#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");
#else
    return (PTR_VOID)(((PTR_UInt8)controlPC) + 1);
#endif
}
PTR_VOID StackFrameIterator::AdjustReturnAddressBackward(PTR_VOID controlPC)
{
#ifdef _TARGET_ARM_
    return (PTR_VOID)(((PTR_UInt8)controlPC) - 2);
#elif defined(_TARGET_ARM64_)
    PORTABILITY_ASSERT("@TODO: FIXME:ARM64");
#else
    return (PTR_VOID)(((PTR_UInt8)controlPC) - 1);
#endif
}

// Given a return address, determine the category of function where it resides.  In
// general, return addresses encountered by the stack walker are required to reside in
// managed code unless they reside in one of the well-known assembly thunks.

// static
StackFrameIterator::ReturnAddressCategory StackFrameIterator::CategorizeUnadjustedReturnAddress(PTR_VOID returnAddress)
{
#if defined(CORERT) // @TODO: CORERT: no portable thunks are defined

    return InManagedCode;

#else // defined(CORERT)

#if defined(FEATURE_DYNAMIC_CODE)
    if (EQUALS_CODE_ADDRESS(returnAddress, ReturnFromCallDescrThunk))
    {
        return InCallDescrThunk;
    }
    else if (EQUALS_CODE_ADDRESS(returnAddress, ReturnFromUniversalTransition))
    {
        return InUniversalTransitionThunk;
    }
#endif

    if (EQUALS_CODE_ADDRESS(returnAddress, RhpThrowEx2) ||
        EQUALS_CODE_ADDRESS(returnAddress, RhpThrowHwEx2) ||
        EQUALS_CODE_ADDRESS(returnAddress, RhpRethrow2))
    {
        return InThrowSiteThunk; 
    }

    if (
#ifdef _TARGET_X86_
        EQUALS_CODE_ADDRESS(returnAddress, RhpCallFunclet2)
#else
        EQUALS_CODE_ADDRESS(returnAddress, RhpCallCatchFunclet2) ||
        EQUALS_CODE_ADDRESS(returnAddress, RhpCallFinallyFunclet2) ||
        EQUALS_CODE_ADDRESS(returnAddress, RhpCallFilterFunclet2)
#endif
        )
    {
        return InFuncletInvokeThunk;
    }

    if (EQUALS_CODE_ADDRESS(returnAddress, ReturnFromManagedCallout2))
    {
        return InManagedCalloutThunk;
    }

    return InManagedCode;
#endif // defined(CORERT)
}

#ifndef DACCESS_COMPILE

COOP_PINVOKE_HELPER(Boolean, RhpSfiInit, (StackFrameIterator* pThis, PAL_LIMITED_CONTEXT* pStackwalkCtx))
{
    Thread * pCurThread = ThreadStore::GetCurrentThread();

    // The stackwalker is intolerant to hijacked threads, as it is largely expecting to be called from C++
    // where the hijack state of the thread is invariant.  Because we've exposed the iterator out to C#, we 
    // need to unhijack every time we callback into C++ because the thread could have been hijacked during our
    // time exectuing C#.
    pCurThread->Unhijack();

    // Passing NULL is a special-case to request a standard managed stack trace for the current thread.
    if (pStackwalkCtx == NULL)
        pThis->InternalInitForStackTrace();
    else
        pThis->InternalInitForEH(pCurThread, pStackwalkCtx);

    bool isValid = pThis->IsValid();
    if (isValid)
        pThis->CalculateCurrentMethodState();
    return isValid ? Boolean_true : Boolean_false;
}

COOP_PINVOKE_HELPER(Boolean, RhpSfiNext, (StackFrameIterator* pThis, UInt32* puExCollideClauseIdx, Boolean* pfUnwoundReversePInvoke))
{
    // The stackwalker is intolerant to hijacked threads, as it is largely expecting to be called from C++
    // where the hijack state of the thread is invariant.  Because we've exposed the iterator out to C#, we 
    // need to unhijack every time we callback into C++ because the thread could have been hijacked during our
    // time exectuing C#.
    ThreadStore::GetCurrentThread()->Unhijack();

    const UInt32 MaxTryRegionIdx = 0xFFFFFFFF;

    ExInfo * pCurExInfo = pThis->m_pNextExInfo;
    pThis->Next();
    bool isValid = pThis->IsValid();
    if (isValid)
        pThis->CalculateCurrentMethodState();

    if (pThis->m_dwFlags & StackFrameIterator::ExCollide)
    {
        ASSERT(pCurExInfo->m_idxCurClause != MaxTryRegionIdx);
        *puExCollideClauseIdx = pCurExInfo->m_idxCurClause;
        pCurExInfo->m_kind = (ExKind)(pCurExInfo->m_kind | EK_SuperscededFlag);
    }
    else
    {
        *puExCollideClauseIdx = MaxTryRegionIdx;
    }

    *pfUnwoundReversePInvoke = (pThis->m_dwFlags & StackFrameIterator::UnwoundReversePInvoke) 
                                    ? Boolean_true 
                                    : Boolean_false;
    return isValid;
}

#endif // !DACCESS_COMPILE
