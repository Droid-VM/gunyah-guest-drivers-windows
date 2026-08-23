/*
 * viostor restricted-DMA-pool (rdmapool) support: virtio-blk-specific staging
 * on top of the shared rdmapool client library (rdmapool/rdmaclient.c).
 * See viostor_rdma.h for the design overview.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "virtio_stor_hw_helper.h" /* brings storport, osdep, virtio_stor.h, srbhelper, SRB_* macros */
#include "virtio_stor_utils.h"

/* ------------------------------------------------------------------ */
/* rdmapool connect / disconnect                                      */
/* ------------------------------------------------------------------ */

NTSTATUS VioStorConnectRdmaPool(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    NTSTATUS status;

    if (adaptExt->dump_mode)
    {
        adaptExt->rdma.Active = FALSE;
        return STATUS_NOT_SUPPORTED;
    }

    /* Meta = one control slot per outstanding request (queue_depth). */
    status = RdmaClientConnect(&adaptExt->rdma,
                               "viostor",
                               adaptExt->pageAllocationSize / PAGE_SIZE,
                               adaptExt->queue_depth * BOUNCE_CTL_PAGES);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    /* Redirect the page bump allocator (used by virtio_find_queues) at the pool. */
    adaptExt->pageAllocationVa = adaptExt->rdma.BaseVA;
    adaptExt->pageAllocationSize = (ULONG)adaptExt->rdma.Size;
    adaptExt->pageOffset = 0;
    return STATUS_SUCCESS;
}

VOID VioStorDisconnectRdmaPool(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    RdmaClientDisconnect(&adaptExt->rdma);
}

/* ------------------------------------------------------------------ */
/* VA <-> PA                                                          */
/* ------------------------------------------------------------------ */

PHYSICAL_ADDRESS VioStorRdmaVAtoPA(PVOID DeviceExtension, PVOID va)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    return RdmaClientVAtoPA(&adaptExt->rdma, va);
}

PVOID VioStorRdmaPAtoVA(PVOID DeviceExtension, PHYSICAL_ADDRESS pa)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    return RdmaClientPAtoVA(&adaptExt->rdma, pa);
}

/* ------------------------------------------------------------------ */
/* Bounce init / build / complete (virtio-blk layout)                 */
/* ------------------------------------------------------------------ */

NTSTATUS VioStorBounceInit(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    ULONG chunk = BOUNCE_DATA_CHUNK_SIZE;

    if (!adaptExt->rdma.Active)
    {
        return STATUS_NOT_SUPPORTED;
    }

    /* Only clamp to size_max when the device actually advertises a segment limit;
     * info.size_max defaults to PAGE_SIZE when the feature is absent, which would
     * needlessly shatter a contiguous chunk into per-page descriptors. */
    if (CHECKBIT(adaptExt->features, VIRTIO_BLK_F_SIZE_MAX) && adaptExt->info.size_max &&
        adaptExt->info.size_max < chunk)
    {
        chunk = adaptExt->info.size_max;
    }

    return RdmaClientBounceInit(&adaptExt->rdma,
                                (PUCHAR)adaptExt->pageAllocationVa + adaptExt->pageOffset,
                                adaptExt->queue_depth ? adaptExt->queue_depth : 64,
                                BOUNCE_CTL_SIZE,
                                0, /* no event area for virtio-blk */
                                chunk);
}

PVOID VioStorBounceAllocCtl(PVOID DeviceExtension, PVOID srbExtArg)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    PSRB_EXTENSION srbExt = (PSRB_EXTENSION)srbExtArg;
    PVOID ctl;

    if (!adaptExt->rdma.Active || !adaptExt->rdma.BounceInitialized)
    {
        return NULL;
    }
    ctl = RdmaClientAllocCtl(&adaptExt->rdma);
    if (ctl == NULL)
    {
        return NULL;
    }
    srbExt->bounceCtl = ctl;
    srbExt->bounceChunkCount = 0;
    RtlCopyMemory((PUCHAR)ctl + BOUNCE_CTL_OUTHDR_OFFSET, &srbExt->vbr.out_hdr, sizeof(srbExt->vbr.out_hdr));
    return ctl;
}

BOOLEAN VioStorBounceBuild(PVOID DeviceExtension, PVOID SrbArg)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    PSRB_TYPE Srb = (PSRB_TYPE)SrbArg;
    PSRB_EXTENSION srbExt = SRB_EXTENSION(Srb);
    PRDMA_CLIENT c = &adaptExt->rdma;
    PVOID ctl;
    PVOID dataVA = NULL;
    ULONG dataLen = SRB_DATA_TRANSFER_LENGTH(Srb);
    BOOLEAN isWrite = (srbExt->vbr.out_hdr.type == VIRTIO_BLK_T_OUT);
    ULONG nChunks, sgIdx, off;

    ctl = VioStorBounceAllocCtl(DeviceExtension, srbExt);
    if (ctl == NULL)
    {
        DbgPrint(" bounce: no ctl slot\n");
        return FALSE;
    }

    if (dataLen)
    {
        if (StorPortGetSystemAddress(DeviceExtension, (PSCSI_REQUEST_BLOCK)Srb, &dataVA) != STOR_STATUS_SUCCESS ||
            dataVA == NULL)
        {
            DbgPrint(" bounce: StorPortGetSystemAddress failed\n");
            RdmaClientFreeCtl(c, ctl);
            srbExt->bounceCtl = NULL;
            return FALSE;
        }
    }
    srbExt->srbDataVA = (PUCHAR)dataVA;
    srbExt->srbDataLen = dataLen;

    nChunks = dataLen ? ((dataLen + c->DataChunkSize - 1) / c->DataChunkSize) : 0;

    /* sg[0] = out_hdr (ctl), sg[1..nChunks] = data chunks, sg[last] = status (ctl). */
    srbExt->sg[0].physAddr = VioStorRdmaVAtoPA(DeviceExtension, (PUCHAR)ctl + BOUNCE_CTL_OUTHDR_OFFSET);
    srbExt->sg[0].length = sizeof(srbExt->vbr.out_hdr);

    off = 0;
    for (sgIdx = 1; sgIdx <= nChunks; sgIdx++)
    {
        PVOID chunk = RdmaClientAllocChunk(c);
        ULONG clen;
        if (chunk == NULL)
        {
            /* Roll back chunks already taken, then the ctl slot. */
            ULONG k;
            for (k = 1; k < sgIdx; k++)
            {
                RdmaClientFreeChunk(c, VioStorRdmaPAtoVA(DeviceExtension, srbExt->sg[k].physAddr));
            }
            RdmaClientFreeCtl(c, ctl);
            srbExt->bounceCtl = NULL;
            srbExt->bounceChunkCount = 0;
            DbgPrint(" bounce: no data chunk\n");
            return FALSE;
        }
        clen = min(c->DataChunkSize, dataLen - off);
        if (isWrite && dataVA)
        {
            RtlCopyMemory(chunk, (PUCHAR)dataVA + off, clen);
        }
        srbExt->sg[sgIdx].physAddr = VioStorRdmaVAtoPA(DeviceExtension, chunk);
        srbExt->sg[sgIdx].length = clen;
        off += clen;
    }
    srbExt->bounceChunkCount = nChunks;

    srbExt->sg[sgIdx].physAddr = VioStorRdmaVAtoPA(DeviceExtension, (PUCHAR)ctl + BOUNCE_CTL_STATUS_OFFSET);
    srbExt->sg[sgIdx].length = sizeof(srbExt->vbr.status);

    if (isWrite)
    {
        srbExt->out = 1 + nChunks;
        srbExt->in = 1;
    }
    else
    {
        srbExt->out = 1;
        srbExt->in = 1 + nChunks;
    }
    return TRUE;
}

VOID VioStorBounceComplete(PVOID DeviceExtension, PVOID srbExtArg)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    PSRB_EXTENSION srbExt = (PSRB_EXTENSION)srbExtArg;
    PRDMA_CLIENT c = &adaptExt->rdma;
    PVOID ctl = srbExt->bounceCtl;
    ULONG i, off;

    if (ctl == NULL)
    {
        return; /* not a bounced request */
    }

    /* Latch device status from the bounce control slot. */
    srbExt->vbr.status = *((u8 *)((PUCHAR)ctl + BOUNCE_CTL_STATUS_OFFSET));

    /* GET_ID: copy the serial out of the bounce slot. */
    if (srbExt->vbr.out_hdr.type == VIRTIO_BLK_T_GET_ID)
    {
        RtlCopyMemory(adaptExt->sn, (PUCHAR)ctl + BOUNCE_CTL_SN_OFFSET, sizeof(adaptExt->sn));
    }

    /* Reads: copy data back from the bounce chunks into the original SRB buffer. */
    if (srbExt->vbr.out_hdr.type == VIRTIO_BLK_T_IN && srbExt->srbDataVA && srbExt->bounceChunkCount)
    {
        off = 0;
        for (i = 1; i <= srbExt->bounceChunkCount; i++)
        {
            PVOID chunk = VioStorRdmaPAtoVA(DeviceExtension, srbExt->sg[i].physAddr);
            ULONG clen = srbExt->sg[i].length;
            RtlCopyMemory((PUCHAR)srbExt->srbDataVA + off, chunk, clen);
            off += clen;
        }
    }

    /* Free chunks then the control slot. */
    for (i = 1; i <= srbExt->bounceChunkCount; i++)
    {
        RdmaClientFreeChunk(c, VioStorRdmaPAtoVA(DeviceExtension, srbExt->sg[i].physAddr));
    }
    RdmaClientFreeCtl(c, ctl);
    srbExt->bounceCtl = NULL;
    srbExt->bounceChunkCount = 0;
}

/* ------------------------------------------------------------------ */
/* Completion poll thread callbacks                                   */
/* ------------------------------------------------------------------ */

static BOOLEAN VioStorPollBusy(PVOID Context)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)Context;
    if (adaptExt->reset_in_progress_count || adaptExt->removed || adaptExt->stopped)
    {
        return FALSE;
    }
    return InterlockedCompareExchange(&adaptExt->outstandingRequests, 0, 0) >= VIOSTOR_POLL_ENTER_QD;
}

static VOID VioStorPollDrain(PVOID Context)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)Context;
    ULONG q;
    for (q = 0; q < adaptExt->num_queues; q++)
    {
        VioStorCompleteRequest(adaptExt, q + adaptExt->msix_has_config_vector, FALSE);
    }
}

NTSTATUS VioStorStartPollThread(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    if (adaptExt->rdma.Tag == NULL)
    {
        adaptExt->rdma.Tag = "viostor";
    }
    return RdmaClientStartPoll(&adaptExt->rdma, VioStorPollBusy, VioStorPollDrain, adaptExt, adaptExt->pollIntervalUs);
}

VOID VioStorStopPollThread(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    RdmaClientStopPoll(&adaptExt->rdma);
}

VOID VioStorPollKick(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    RdmaClientPollKick(&adaptExt->rdma);
}
