/*
 * viostor restricted-DMA-pool (rdmapool) support: virtio-blk-specific staging
 * on top of the shared rdmapool client library (rdmapool/rdmaclient.c), which
 * provides the pool connection, the SLIST bounce allocator (control slots +
 * large contiguous data chunks) and the completion poll thread.
 *
 * This layer owns what is virtio-blk-shaped: the control-slot layout
 * (out_hdr / status / serial), how a read/write SRB is staged through data
 * chunks, and which queues the poll thread drains.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef _VIOSTOR_RDMA_H_
#define _VIOSTOR_RDMA_H_

#include "rdmaclient.h"

/*
 * Control slot layout (one page). Holds the device-visible request metadata
 * that must live in rdmapool: out_hdr, status, the serial buffer (GET_ID) and
 * the discard segment list (UNMAP). Indirect descriptors are disabled on the
 * rdmapool path, so no indirect table is needed here.
 */
#define BOUNCE_CTL_PAGES          1
#define BOUNCE_CTL_SIZE           (BOUNCE_CTL_PAGES * PAGE_SIZE)
#define BOUNCE_CTL_OUTHDR_OFFSET  0
#define BOUNCE_CTL_STATUS_OFFSET  16 /* sizeof(blk_outhdr) = u32 + u32 + u64 */
#define BOUNCE_CTL_SN_OFFSET      32 /* BLOCK_SERIAL_STRLEN (20) bytes */
#define BOUNCE_CTL_DISCARD_OFFSET 64 /* MAX_DISCARD_SEGMENTS (16) x 16-byte blk_discard_write_zeroes = 256 bytes */

/*
 * Default contiguous data chunk size. Large enough that typical transfers map
 * to a handful of descriptors (1MB -> ceil(1MB/chunk)), not 256 per-page
 * descriptors. Clamped down to the device size_max at init.
 */
#define BOUNCE_DATA_CHUNK_SIZE    (256 * 1024)

/* Default gentle poll interval; registry PollIntervalUs overrides (0 = tight spin). */
#define VIOSTOR_POLL_INTERVAL_US  RDMA_CLIENT_POLL_INTERVAL_US
#define VIOSTOR_POLL_ENTER_QD     2

/*
 * Connect to the rdmapool driver and allocate one contiguous region big enough
 * for the vrings (adaptExt->pageAllocationSize, already computed) plus a bounce
 * area. On success sets adaptExt->rdma.Active and redirects the page bump
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
 * Called from VirtIoBuildIo when rdma.Active, AFTER lba/sgList validation.
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
