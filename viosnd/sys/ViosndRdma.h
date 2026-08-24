/*
 * viosnd restricted-DMA-pool (rdmapool) support.
 *
 * In a Gunyah protected VM the guest's own pages are lent, not shared: crosvm
 * can only read and write memory inside the restricted DMA pool. Everything
 * the virtio-snd device touches -- the vrings, the control/event messages and
 * the PCM payload staged into VIRTIO_SND_PCM_XFER requests -- therefore has to
 * live in the pool.
 *
 * viosnd makes that cheap. Every device-visible byte already comes out of one
 * function (ViosndAllocateDmaBuffer) and the WaveRT buffer the Windows audio
 * engine writes into is NOT device-visible -- ViosndWritePcm copies each period
 * from it into a DMA buffer before the descriptor is added. So the whole port
 * is: point that one allocator at the pool.
 *
 * Sub-allocation is page-granular, over however many regions the driver has
 * taken. Unlike the storage miniports (fixed control slots + fixed data
 * chunks, sized by queue depth) viosnd asks for a handful of variable-sized
 * blocks -- vrings at device init, then one IO pool per stream at stream start
 * -- so a bitmap fits better than a SLIST of fixed slots.
 *
 * Absent pool = absent device interface: RdmaClientOpen returns
 * STATUS_NOT_FOUND, the pool stays closed, and the driver keeps the ordinary
 * AllocateCommonBuffer path. That is what runs on QEMU/KVM and on a
 * pseudo-unprotected Gunyah VM, where guest RAM is shared with the host and
 * there is nothing to stage through.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef _VIOSNDRDMA_H_
#define _VIOSNDRDMA_H_

extern "C"
{
#include "rdmaclient.h"
}

/*
 * Regions are taken from the pool as they are needed rather than reserved up
 * front, for two reasons that turn out to be the same reason.
 *
 * The size is not knowable in advance. Rings and control buffers can be
 * computed exactly -- the advertised queue sizes and the stream count are both
 * plain reads from the device before anything is allocated -- but a stream's
 * staging is sized from the WaveRT buffer the OS asks for, which does not
 * exist until the stream is opened. Reserving for it means reserving for the
 * largest format the driver would ever offer, which is several times what any
 * particular stream uses.
 *
 * And the pool hands out contiguous runs, so a single large request can be
 * refused against free space that would satisfy several smaller ones. viostor
 * takes half the pool outright and NetKVM grows into it a buffer at a time; by
 * the time a second sound card starts there may be no run left of the size a
 * one-region driver needs, while the pieces it actually wants all fit.
 */
#define VIOSND_RDMA_MAX_REGIONS        16u

/*
 * The least a region is worth taking. A page-at-a-time driver would fragment
 * the shared pool for everyone else, so requests are rounded up to this and the
 * remainder is left for the next allocation from the same region.
 */
#define VIOSND_RDMA_REGION_GRAIN_PAGES 32u

typedef struct _VIOSND_RDMA_REGION
{
    PVOID BaseVA;
    PHYSICAL_ADDRESS BasePA;
    ULONG Pages;
    RTL_BITMAP Bitmap;
    PULONG BitmapBuffer;
} VIOSND_RDMA_REGION, *PVIOSND_RDMA_REGION;

typedef struct _VIOSND_RDMA
{
    RDMA_CLIENT Client;
    /* Guards the region table and every bitmap in it. Allocation happens at
     * PASSIVE_LEVEL (device init and stream start); the lock is still taken at
     * DISPATCH so a future caller on the streaming path cannot corrupt it. */
    KSPIN_LOCK Lock;
    VIOSND_RDMA_REGION Regions[VIOSND_RDMA_MAX_REGIONS];
    ULONG RegionCount;
    BOOLEAN Opened;
} VIOSND_RDMA, *PVIOSND_RDMA;

/*
 * Open the pool. Returns STATUS_NOT_FOUND when rdmapool is absent, which
 * callers treat as "stay on normal DMA" -- that is what an unprotected or
 * pseudo-unprotected VM looks like. Any other failure means the pool is there
 * and unusable, and normal DMA would not be host-readable.
 *
 * No memory is taken here; regions arrive with the first allocation.
 */
NTSTATUS ViosndRdmaOpen(_Inout_ PVIOSND_RDMA Rdma);

VOID ViosndRdmaClose(_Inout_ PVIOSND_RDMA Rdma);

/* TRUE once the pool is open and every DMA buffer must come from it. */
__forceinline BOOLEAN ViosndRdmaActive(_In_ PVIOSND_RDMA Rdma)
{
    return Rdma->Opened;
}

/*
 * Allocate Size bytes (rounded up to pages) and report the physical address the
 * device must be given. Takes a new region from the pool when no existing one
 * has a run long enough. NULL when the pool cannot provide it.
 */
_Ret_maybenull_ PVOID ViosndRdmaAlloc(_Inout_ PVIOSND_RDMA Rdma,
                                      _In_ SIZE_T Size,
                                      _Out_ PPHYSICAL_ADDRESS LogicalAddress);

VOID ViosndRdmaFree(_Inout_ PVIOSND_RDMA Rdma, _In_opt_ PVOID Va, _In_ SIZE_T Size);

/* Pages currently held across all regions, for diagnostics. */
ULONG ViosndRdmaHeldPages(_In_ PVIOSND_RDMA Rdma);

#endif /* _VIOSNDRDMA_H_ */
