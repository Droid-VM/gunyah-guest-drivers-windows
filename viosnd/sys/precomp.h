#pragma once

#include <ntddk.h>
#include <wdm.h>
#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>
#include <ntstrsafe.h>

#include "trace.h"
#include "viosnd.h"
#include "ViosndPcm.h"
#include "ViosndFormat.h"
#include "ViosndEndpoint.h"
#include "ViosndVirtio.h"
#include "ViosndTopology.h"
#include "ViosndWaveRT.h"

/* Verbose tracing, off by default as it is upstream. It was on while TX starvation
 * was under investigation; that question is answered, and a DbgPrint on the audio
 * engine's own callback path is not something to ship on the strength of "it costs
 * nothing when nobody is looking".
 *
 * What a machine that is merely running still gets is the registry diagnostics
 * (ViosndRecordDiag), which need no debugger and are written once per state
 * transition rather than on a timer. Build with -DVIOSND_ENABLE_LOG=1 to get the
 * per-loop detail back under a debugger or DebugView. */
#ifndef VIOSND_ENABLE_LOG
#define VIOSND_ENABLE_LOG 0
#endif

#if VIOSND_ENABLE_LOG
#define VIOSND_LOG(...) DbgPrintEx(__VA_ARGS__)
#else
#define VIOSND_LOG(...) ((void)0)
#endif

inline void *__cdecl operator new(size_t, void *Address)
{
    return Address;
}

inline void __cdecl operator delete(void *, void *)
{
}
