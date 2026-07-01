/*
 * viostor restricted-DMA-pool (rdmapool) support + bounce allocator + poll thread.
 *
 * In a Gunyah protected VM the virtio-blk backend can only touch memory that
 * lives in the restricted DMA pool (rdmapool). Normal guest pages handed to us
 * by StorPort are NOT device-visible, so every byte the device reads or writes
 * (vrings, out_hdr, status, and the I/O data itself) must be staged through
 * rdmapool memory. This module provides, in one place ("rdmapool allocator
 * unified management"):
 *
 *   - Connect/disconnect to the rdmapool driver (mirrors NetKVM ParaNdis_RdmaPool
 *     and the proven viostor pVM path), allocating one contiguous region that
 *     backs both the vrings and the bounce buffers.
 *   - A lock-free (SLIST) sub-allocator carving that region into fixed control
 *     slots (out_hdr + status) and large CONTIGUOUS data chunks. A chunk maps to
 *     a single virtqueue descriptor, so a large transfer costs ceil(len/chunk)
 *     descriptors instead of one-per-4KB-page.
 *   - A completion POLL THREAD that drains the used rings on a ~1ms cadence and
 *     blocks when idle, replacing the old inline busy-poll that wedged the
 *     StartIo thread. The hardware ISR stays the fast path; the poll thread is
 *     the deep-idle safety net for this platform's interrupt-wake latency.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef _VIOSTOR_RDMA_H_
#define _VIOSTOR_RDMA_H_

/*
 * Control slot layout (one page). Holds the device-visible request metadata that
 * must live in rdmapool: out_hdr, status, and (for GET_ID) the serial buffer.
 * Indirect descriptors are disabled on the rdmapool path, so no indirect table
 * is needed here. DISCARD is not negotiated on this path, so no discard area.
 */
#define BOUNCE_CTL_PAGES         1
#define BOUNCE_CTL_SIZE          (BOUNCE_CTL_PAGES * PAGE_SIZE)
#define BOUNCE_CTL_OUTHDR_OFFSET 0
#define BOUNCE_CTL_STATUS_OFFSET 16 /* sizeof(blk_outhdr) = u32 + u32 + u64 */
#define BOUNCE_CTL_SN_OFFSET     32 /* BLOCK_SERIAL_STRLEN (20) bytes */

/*
 * Default contiguous data chunk size. Large enough that typical transfers map to
 * a handful of descriptors (1MB -> ceil(1MB/chunk)), not 256 per-page descriptors.
 * Clamped down to the device size_max at init.
 */
#define BOUNCE_DATA_CHUNK_SIZE   (256 * 1024)

/*
 * Poll thread cadence. Adaptive: while I/O is in flight the thread spin-drains
 * with a short stall between drains (low latency -> high random IOPS, ~1 core
 * burned only during active I/O, never blocking StartIo); when idle it blocks on
 * the wake event with a safety-net timeout (≈0 CPU).
 */
#define VIOSTOR_POLL_SPIN_US     10  /* tight-spin stall between drains (PollIntervalUs==0) */
#define VIOSTOR_POLL_IDLE_MS     100 /* idle safety-net wakeup */
#define VIOSTOR_POLL_INTERVAL_US                                                                                       \
    1000 /* default gentle poll interval (1ms); registry                                                               \
          * PollIntervalUs overrides; 0 selects the tight spin */

typedef struct _BOUNCE_ALLOCATOR
{
    PUCHAR BaseVA;
    PHYSICAL_ADDRESS BasePA;

    /* Control-slot free list (lock-free SLIST), one BOUNCE_CTL_SIZE block each. */
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER CtlFreeList;
    ULONG CtlSlotCount;
    PUCHAR CtlBaseVA;

    /* Data-chunk free list (lock-free SLIST), one contiguous DataChunkSize block each. */
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER DataFreeList;
    ULONG DataChunkSize;
    ULONG DataChunkCount;
    PUCHAR DataBaseVA;

    BOOLEAN Initialized;
} BOUNCE_ALLOCATOR, *PBOUNCE_ALLOCATOR;

/*
 * Connect to the rdmapool driver and allocate one contiguous region big enough
 * for the vrings (adaptExt->pageAllocationSize, already computed) plus a bounce
 * area. On success sets adaptExt->rdmaPoolActive and redirects the page bump
 * allocator (pageAllocationVa/Size/Offset) at the pool. PASSIVE_LEVEL only.
 * Returns STATUS_NOT_FOUND when rdmapool is absent (caller keeps the normal KVM
 * path). Call from VirtIoFindAdapter.
 */
NTSTATUS VioStorConnectRdmaPool(PVOID DeviceExtension);
VOID VioStorDisconnectRdmaPool(PVOID DeviceExtension);

/*
 * Carve the bounce allocator out of the rdmapool region left after the vrings
 * were allocated (i.e. starting at pageAllocationVa + pageOffset). Call once,
 * after virtio_find_queues, at PASSIVE_LEVEL.
 */
NTSTATUS VioStorBounceInit(PVOID DeviceExtension);

/* VA<->PA within the contiguous rdmapool region. */
PHYSICAL_ADDRESS VioStorRdmaVAtoPA(PVOID DeviceExtension, PVOID va);
PVOID VioStorRdmaPAtoVA(PVOID DeviceExtension, PHYSICAL_ADDRESS pa);

/*
 * Stage a read/write SRB through bounce buffers. Fills srbExt->sg[]/out/in with
 * rdmapool physical addresses (out_hdr + data chunks + status), copies write
 * data in, and records what completion needs to copy back/free. Returns FALSE
 * (and completes the SRB with an error) if the pool is exhausted.
 * Called from VirtIoBuildIo when rdmaPoolActive, AFTER lba/sgList validation.
 */
BOOLEAN VioStorBounceBuild(PVOID DeviceExtension, PVOID Srb);

/*
 * Allocate a control slot, copy srbExt->vbr.out_hdr into it, and stash it in
 * srbExt->bounceCtl. Used by the metadata-only requests (flush / get serial)
 * to place out_hdr/status/sn in rdmapool. Returns the slot VA or NULL.
 */
PVOID VioStorBounceAllocCtl(PVOID DeviceExtension, PVOID srbExt);

/*
 * Finish a bounced SRB: for reads copy the data out of the bounce chunks into
 * the original SRB buffer, latch the device status, and free the bounce
 * resources. Called from VioStorCompleteRequest before completing each SRB.
 */
VOID VioStorBounceComplete(PVOID DeviceExtension, PVOID srbExt);

/* Poll thread lifecycle (PASSIVE_LEVEL). */
NTSTATUS VioStorStartPollThread(PVOID DeviceExtension);
VOID VioStorStopPollThread(PVOID DeviceExtension);
/* Wake the poll thread after submitting work (any IRQL). */
VOID VioStorPollKick(PVOID DeviceExtension);

#endif /* _VIOSTOR_RDMA_H_ */
