/*
 * rdmapool client library — shared by the Gunyah protected-VM storage
 * miniports (viostor, vioscsi).
 *
 * In a Gunyah protected VM the virtio backend can only touch memory that
 * lives in the restricted DMA pool (rdmapool). Normal guest pages are NOT
 * device-visible, so vrings, request/response headers and the I/O data
 * itself must be staged through rdmapool memory. This module provides the
 * driver-independent mechanics, in one place:
 *
 *   - Connect/disconnect to the rdmapool driver, allocating one contiguous
 *     region that backs both the vrings and the bounce buffers, and
 *     VA<->PA translation within it.
 *   - A lock-free (SLIST) sub-allocator carving the region into fixed
 *     CONTROL SLOTS (device-visible request metadata; the per-driver layout
 *     inside a slot is the caller's business) and large CONTIGUOUS DATA
 *     CHUNKS (a chunk maps to a single virtqueue descriptor, so a transfer
 *     costs ceil(len/chunk) descriptors instead of one per 4KB page), plus
 *     an optional reserved EVENT AREA (vioscsi event buffers).
 *   - A completion POLL THREAD whose driver callback selects when polling is
 *     the fast path. Below that threshold the thread blocks and interrupts
 *     handle completions. This avoids polling sparse I/O while retaining the
 *     throughput workaround for deep-idle interrupt-wake latency.
 *
 * What stays in each driver: how a request is staged (virtio-blk out_hdr /
 * status vs. virtio-scsi req / resp unions), which queues to drain and under
 * which locks, and all StorPort interaction.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef _RDMACLIENT_H_
#define _RDMACLIENT_H_

/* Poll thread cadence defaults (see RdmaClientStartPoll). */
#define RDMA_CLIENT_POLL_SPIN_US      10  /* tight-spin stall between drains (PollIntervalUs==0) */
#define RDMA_CLIENT_POLL_BURST_US     100 /* short poll after an idle-to-busy transition */
#define RDMA_CLIENT_POLL_IDLE_MS      100 /* idle safety-net wakeup */
#define RDMA_CLIENT_POLL_INTERVAL_US 1000

/* Return TRUE while polling should be the completion fast path. */
typedef BOOLEAN (*RDMA_CLIENT_BUSY_CB)(PVOID Context);
/* Drain all completion queues once. Runs at PASSIVE_LEVEL on the poll thread;
 * must acquire/release whatever locks the driver's ISR path needs. */
typedef VOID (*RDMA_CLIENT_DRAIN_CB)(PVOID Context);

typedef struct _RDMA_CLIENT
{
    /* --- pool connection (RdmaClientConnect) --- */
    BOOLEAN Active;
    const char *Tag; /* short driver name for DbgPrint */
    PDEVICE_OBJECT PoolDeviceObject;
    PFILE_OBJECT PoolFileObject;
    PVOID BaseVA;
    PHYSICAL_ADDRESS BasePA;
    ULONG64 Size;

    /* --- bounce sub-allocator (RdmaClientBounceInit) --- */
    PUCHAR EventBaseVA; /* reserved EventBytes area, or NULL */
    ULONG EventBytes;
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER CtlFreeList;
    ULONG CtlSlotSize;
    ULONG CtlSlotCount;
    PUCHAR CtlBaseVA;
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER DataFreeList;
    ULONG DataChunkSize;
    ULONG DataChunkCount;
    PUCHAR DataBaseVA;
    BOOLEAN BounceInitialized;

    /* --- completion poll thread (RdmaClientStartPoll) --- */
    PVOID PollThread;       /* PKTHREAD referenced object */
    KEVENT PollWake;        /* signalled by the submit path via RdmaClientPollKick */
    volatile LONG PollStop; /* set to 1 to ask the thread to exit */
    volatile LONG PollActive;
    ULONG PollIntervalUs;   /* us to sleep between drains while busy; 0 = tight spin */
    RDMA_CLIENT_BUSY_CB BusyCb;
    RDMA_CLIENT_DRAIN_CB DrainCb;
    PVOID CbContext;
} RDMA_CLIENT, *PRDMA_CLIENT;

/*
 * Connect to the rdmapool driver and allocate one contiguous region of
 * RingPages (the caller's vrings) + MetaPages (control slots / event area)
 * + a bounce data area capped at 32MB / half the pool (the pool is shared by
 * all pVM drivers), clamped to the pool size. On success sets
 * Active/BaseVA/BasePA/Size; the caller redirects its vring page allocator
 * at BaseVA. PASSIVE_LEVEL only. Returns STATUS_NOT_FOUND when rdmapool is
 * absent, so the caller keeps the normal (KVM/QEMU) DMA path.
 */
NTSTATUS RdmaClientConnect(PRDMA_CLIENT c, const char *Tag, ULONG RingPages, ULONG MetaPages);
VOID RdmaClientDisconnect(PRDMA_CLIENT c);

/* VA<->PA within the contiguous rdmapool region. */
PHYSICAL_ADDRESS RdmaClientVAtoPA(PRDMA_CLIENT c, PVOID va);
PVOID RdmaClientPAtoVA(PRDMA_CLIENT c, PHYSICAL_ADDRESS pa);
/* TRUE if va lies inside the connected region (physical-address override hook). */
BOOLEAN RdmaClientOwnsVA(PRDMA_CLIENT c, PVOID va);

/*
 * Carve the bounce allocator out of the region left after the vrings,
 * starting at FreeStart (page-aligned up) and running to the end of the
 * region: [EventBytes] [CtlSlots x CtlSlotSize] [data chunks x DataChunkSize].
 * CtlSlots is clamped to half the space; DataChunkSize (page-multiple) is
 * shrunk until there is at least one chunk per control slot (concurrency).
 * Idempotent: safe to call again on adapter re-initialization once all
 * outstanding requests are gone.
 */
NTSTATUS RdmaClientBounceInit(PRDMA_CLIENT c,
                              PVOID FreeStart,
                              ULONG CtlSlots,
                              ULONG CtlSlotSize,
                              ULONG EventBytes,
                              ULONG DataChunkSize);

/* Lock-free slot/chunk alloc/free (any IRQL). Return NULL when exhausted. */
PVOID RdmaClientAllocCtl(PRDMA_CLIENT c);
VOID RdmaClientFreeCtl(PRDMA_CLIENT c, PVOID slot);
PVOID RdmaClientAllocChunk(PRDMA_CLIENT c);
VOID RdmaClientFreeChunk(PRDMA_CLIENT c, PVOID chunk);

/*
 * Start the completion poll thread (PASSIVE_LEVEL). This helper is independent
 * of whether the restricted DMA pool is active. After an idle-to-busy
 * transition selected by BusyCb it polls for RDMA_CLIENT_POLL_BURST_US, then
 * sleeps PollIntervalUs between drains (default 1ms gentle poll; 0 = tight
 * polling for max IOPS). When BusyCb returns FALSE, completions are handled by
 * interrupts and the thread blocks with a RDMA_CLIENT_POLL_IDLE_MS safety-net
 * timeout (~0 CPU).
 */
NTSTATUS RdmaClientStartPoll(PRDMA_CLIENT c,
                             RDMA_CLIENT_BUSY_CB BusyCb,
                             RDMA_CLIENT_DRAIN_CB DrainCb,
                             PVOID CbContext,
                             ULONG PollIntervalUs);
VOID RdmaClientStopPoll(PRDMA_CLIENT c);
/* Wake the poll thread after submitting work (any IRQL). */
VOID RdmaClientPollKick(PRDMA_CLIENT c);

#endif /* _RDMACLIENT_H_ */
