// MinGW-w64 compatibility shim for the EfiDSEFix sources.
//
// Force-included (-include) ahead of every translation unit by
// scripts/build-windows.sh. Everything here compensates for MSVC-only
// constructs or for MinGW header differences; none of it changes behaviour.
//
// Kept in a separate header (rather than editing the upstream sources) so the
// fork stays close to Mattiwatti/EfiGuard and rebases cleanly.
#pragma once

#if defined(__MINGW32__)

// Pull in <windows.h> with the same prelude ntdll.h uses, so that including it
// here (out of order, before ntdll.h gets a chance to) is equivalent.
//
// WIN32_NO_STATUS stops winnt.h from defining STATUS_*; ntdll.h needs those to
// come from <ntstatus.h> instead, and the two definitions differ.
#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// SAL annotation. MinGW's sal.h defines _Return_type_success_ only in some
// configurations; make sure it is a no-op macro either way.
#include <sal.h>
#ifndef _Return_type_success_
#define _Return_type_success_(expr)
#endif

// MSVC compiler intrinsics with no MinGW equivalent. Both are used exclusively
// for annotations and debug-only analysis, so compiling them away is correct.
#ifndef __noop
#define __noop(...) ((void)0)
#endif
#ifndef __annotation
#define __annotation(...) ((void)0)
#endif

// winnt.h gates PROCESSOR_NUMBER behind _WIN32_WINNT >= 0x0601, but the
// project deliberately targets Vista (_WIN32_WINNT == 0x0600) and ntdll.h
// needs the type for SYSTEM_THREAD_INFORMATION. Define it without raising the
// target version, so we do not accidentally gain access to Win7-only APIs.
#if !defined(___PROCESSOR_NUMBER_DEFINED)
typedef struct _PROCESSOR_NUMBER {
	WORD Group;
	BYTE Number;
	BYTE Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;

#define ALL_PROCESSOR_GROUPS 0xffff
#define ___PROCESSOR_NUMBER_DEFINED
#endif

#endif // __MINGW32__
