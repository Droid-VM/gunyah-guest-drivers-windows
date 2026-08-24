/*
 * viofs restricted-DMA-pool (rdmapool) support: virtio-fs-shaped staging on
 * top of the shared rdmapool client library (rdmapool/rdmaclient.c).
 *
 * Why viofs needs this at all, when its vrings do not: the WDF layer already
 * routes VirtIOWdfDeviceAllocDmaMemory() through the pool whenever rdmapool is
 * present (VirtIO/WDF/Dma.c), so the vrings and the indirect area land in
 * device-visible memory on their own. The *payload* does not. The DMA
 * transaction path (VirtIOWdfDeviceDmaAsync) hands the WDF DMA enabler an MDL
 * over ordinary guest pages and puts their physical addresses straight into
 * the descriptor chain -- addresses a Gunyah protected VM's host cannot read
 * or write. Linux never had this problem because its bounce happens inside the
 * generic ring code (vring_map_one_sg -> dma_map_page), below every driver.
 * Windows has no such layer, which is why each driver stages for itself:
 * vioscsi and viostor already do, and this is the same thing for virtio-fs.
 *
 * The shape of a virtio-fs request is simpler than a SCSI one: two byte
 * streams, an out (fuse_in_header + op struct + write data) and an in
 * (fuse_out_header + op struct + read data). Each is staged whole -- into one
 * control slot when it is small, into a run of data chunks when it is not --
 * and the in stream is copied back to wherever it came from at completion.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef _VIOFS_RDMA_H_
#define _VIOFS_RDMA_H_

#include "rdmaclient.h"

/*
 * A control slot holds a whole small request: every metadata op (LOOKUP,
 * GETATTR, OPEN, and a short READDIR) fits in a page, and spending a 64KB data
 * chunk on a 128-byte LOOKUP would empty the pool for no reason.
 */
#define VIRTFS_BOUNCE_CTL_SIZE   PAGE_SIZE
#define VIRTFS_BOUNCE_CTL_SLOTS  64

/*
 * One chunk is one descriptor, so a transfer costs ceil(len/chunk) descriptors
 * rather than one per page. 64KB against crosvm's 1MB max_write is 16
 * descriptors for the largest request, against a queue of 1024.
 */
#define VIRTFS_BOUNCE_CHUNK_SIZE (64 * 1024)

/*
 * Enough chunks for one 1MB stream (16) with room to spare. A request that
 * would need more is refused rather than truncated -- see VirtFsBounceBuild.
 */
#define VIRTFS_BOUNCE_MAX_CHUNKS 24

/* Copy-back targets: the generic path has one, the read fast path has two
 * (the fuse_out_header buffer and the caller's data buffer). */
#define VIRTFS_BOUNCE_MAX_SEGS   8

typedef struct _VIRTFS_BOUNCE_SEG
{
    PUCHAR Va;
    ULONG Length;
} VIRTFS_BOUNCE_SEG;

typedef struct _VIRTFS_BOUNCE
{
    BOOLEAN Staged; /* this request went through the pool; drives completion */

    /* Out (device-readable) staging: a ctl slot or a run of chunks. */
    PVOID OutCtl;
    PVOID OutChunks[VIRTFS_BOUNCE_MAX_CHUNKS];
    ULONG OutChunkLen[VIRTFS_BOUNCE_MAX_CHUNKS];
    ULONG OutChunkCount;

    /* In (device-writable) staging, plus where its bytes have to end up. */
    PVOID InCtl;
    PVOID InChunks[VIRTFS_BOUNCE_MAX_CHUNKS];
    ULONG InChunkLen[VIRTFS_BOUNCE_MAX_CHUNKS];
    ULONG InChunkCount;
    ULONG InTotalLength;
    VIRTFS_BOUNCE_SEG InSegs[VIRTFS_BOUNCE_MAX_SEGS];
    ULONG InSegCount;
} VIRTFS_BOUNCE, *PVIRTFS_BOUNCE;

struct _DEVICE_CONTEXT;
struct _VIRTIO_FS_REQUEST;

/*
 * Connect to rdmapool and carve the bounce area. The vrings are NOT requested
 * here: the WDF layer has already taken them from the pool by the time this
 * runs, so asking again would reserve them twice in a pool every pVM driver
 * shares. Returns STATUS_NOT_FOUND when ACPI\RDMA0000 does not exist -- i.e.
 * on QEMU/KVM or an unprotected VM -- and the caller keeps the normal DMA
 * path. PASSIVE_LEVEL only; call after VirtIOWdfInitQueues.
 */
NTSTATUS VirtFsRdmaConnect(struct _DEVICE_CONTEXT *Context);
VOID VirtFsRdmaDisconnect(struct _DEVICE_CONTEXT *Context);

/*
 * Stage a request's two streams into pool memory, fill Request->SGTable with
 * pool physical addresses and report the descriptor counts. Copies the out
 * stream in and records where the in stream has to be copied back to.
 *
 * Returns FALSE when the pool is momentarily exhausted or the request is
 * larger than VIRTFS_BOUNCE_MAX_CHUNKS can hold; everything taken so far is
 * released first, so the caller may simply fail the request. Any IRQL.
 */
BOOLEAN VirtFsBounceBuild(struct _DEVICE_CONTEXT *Context,
                          struct _VIRTIO_FS_REQUEST *Request,
                          ULONG *OutNum,
                          ULONG *InNum);

/*
 * Copy the device-written bytes back to the original buffers and release the
 * staging. `Length` is what the device reported it wrote, so nothing beyond it
 * is copied. A no-op on a request that was not staged, so completion paths may
 * call it unconditionally. Any IRQL.
 */
VOID VirtFsBounceComplete(struct _DEVICE_CONTEXT *Context, struct _VIRTIO_FS_REQUEST *Request, ULONG Length);

/*
 * Release the staging without copying anything back, for a request that failed
 * or was cancelled before the device touched it. Any IRQL.
 */
VOID VirtFsBounceRelease(struct _DEVICE_CONTEXT *Context, struct _VIRTIO_FS_REQUEST *Request);

#endif /* _VIOFS_RDMA_H_ */
