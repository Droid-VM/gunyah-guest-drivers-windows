/*
 * viofs restricted-DMA-pool staging. See viofs_rdma.h for why this exists.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "viofs.h"
#include "viofs_rdma.tmh"

/*
 * How much of the shared pool to reserve for payload staging. 16MB against
 * 64KB chunks is 256 chunks, i.e. sixteen 1MB transfers in flight, and the
 * pool is shared with every other pVM driver in the guest -- this is the knob
 * to turn if VirtFsBounceBuild starts reporting exhaustion. Turning down
 * crosvm's max_write (fuse/src/filesystem.rs MAX_BUFFER_SIZE) is the cheaper
 * lever for the same problem: it shrinks every request instead of buying room
 * for the largest one.
 */
#define VIRTFS_RDMA_DATA_PAGES (4096u)

/* Control slots come out of MetaPages, which must cover all of them. */
#define VIRTFS_RDMA_META_PAGES ((VIRTFS_BOUNCE_CTL_SIZE * VIRTFS_BOUNCE_CTL_SLOTS) / PAGE_SIZE)

NTSTATUS VirtFsRdmaConnect(PDEVICE_CONTEXT Context)
{
    PRDMA_CLIENT c = &Context->Rdma;
    NTSTATUS status;

    /*
     * RingPages is 0 on purpose. VirtIOWdfInitialize() has already connected
     * the WDF layer to rdmapool and taken the vrings and the indirect area
     * from it (VirtIO/WDF/Dma.c: VirtIOWdfDeviceAllocDmaMemory ->
     * AllocateFromRdmaPool), so counting them again here would reserve them
     * twice out of a pool that is shared guest-wide.
     */
    status = RdmaClientConnectEx(c, "viofs", 0, VIRTFS_RDMA_META_PAGES, VIRTFS_RDMA_DATA_PAGES);
    if (!NT_SUCCESS(status))
    {
        /* STATUS_NOT_FOUND simply means no ACPI\RDMA0000: an unprotected VM,
         * or QEMU/KVM. Not an error -- the normal DMA path is correct there. */
        TraceEvents(TRACE_LEVEL_INFORMATION,
                    DBG_POWER,
                    "rdmapool not in use (%!STATUS!); payloads take the normal DMA path",
                    status);
        return status;
    }

    status = RdmaClientBounceInit(c,
                                  c->BaseVA,
                                  VIRTFS_BOUNCE_CTL_SLOTS,
                                  VIRTFS_BOUNCE_CTL_SIZE,
                                  0, /* no event area: virtio-fs has no event queue */
                                  VIRTFS_BOUNCE_CHUNK_SIZE);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_POWER, "RdmaClientBounceInit failed %!STATUS!", status);
        RdmaClientDisconnect(c);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION,
                DBG_POWER,
                "rdmapool active: %u ctl slots of %u, %u data chunks of %u",
                c->CtlSlotCount,
                c->CtlSlotSize,
                c->DataChunkCount,
                c->DataChunkSize);
    return STATUS_SUCCESS;
}

VOID VirtFsRdmaDisconnect(PDEVICE_CONTEXT Context)
{
    if (Context->Rdma.Active)
    {
        RdmaClientDisconnect(&Context->Rdma);
    }
}

/*
 * The segments a DMA transaction's buffer actually consists of.
 *
 * VirtIOWdfDeviceDmaAsync() reads these params two ways and so must we: a
 * WDF_INVALID_HANDLE `req` means `buffer` is an MDL chain (the read fast path
 * chains the fuse_out_header buffer to the caller's data buffer), anything
 * else means it is one flat kernel address of `size` bytes.
 */
static ULONG VirtFsCollectSegs(PVIRTIO_DMA_TRANSACTION_PARAMS Params,
                               VIRTFS_BOUNCE_SEG *Segs,
                               ULONG MaxSegs,
                               ULONG *TotalOut)
{
    ULONG n = 0;
    ULONG total = 0;

    if (Params->req == (WDFREQUEST)WDF_INVALID_HANDLE)
    {
        PMDL mdl = (PMDL)Params->buffer;
        while (mdl != NULL && n < MaxSegs && total < Params->size)
        {
            PUCHAR va = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
            ULONG len = MmGetMdlByteCount(mdl);

            if (va == NULL)
            {
                /* Nothing partially staged yet; the caller fails the request. */
                *TotalOut = 0;
                return 0;
            }
            len = min(len, Params->size - total);
            Segs[n].Va = va;
            Segs[n].Length = len;
            total += len;
            n++;
            mdl = mdl->Next;
        }
    }
    else if (Params->req == NULL)
    {
        Segs[0].Va = (PUCHAR)Params->buffer;
        Segs[0].Length = Params->size;
        total = Params->size;
        n = 1;
    }
    else
    {
        /*
         * The third mode the params allow -- a real WDFREQUEST, where the data
         * lives in a Write IRP and `buffer`/`size` mean nothing. viofs never
         * builds one, and guessing would stage whatever `buffer` happened to
         * hold. Refuse instead of inventing.
         */
        *TotalOut = 0;
        return 0;
    }

    *TotalOut = total;
    return n;
}

/* Copy `Length` bytes out of a segment list, starting at `Offset` into it. */
static VOID VirtFsCopyFromSegs(VIRTFS_BOUNCE_SEG *Segs, ULONG SegCount, ULONG Offset, PUCHAR Dst, ULONG Length)
{
    ULONG i, pos = 0;

    for (i = 0; i < SegCount && Length; i++)
    {
        ULONG segLen = Segs[i].Length;

        if (pos + segLen <= Offset)
        {
            pos += segLen;
            continue;
        }
        {
            ULONG within = (Offset > pos) ? (Offset - pos) : 0;
            ULONG avail = segLen - within;
            ULONG take = min(avail, Length);

            RtlCopyMemory(Dst, Segs[i].Va + within, take);
            Dst += take;
            Length -= take;
            Offset += take;
            pos += segLen;
        }
    }
}

/* Copy `Length` bytes into a segment list, starting at `Offset` into it. */
static VOID VirtFsCopyToSegs(VIRTFS_BOUNCE_SEG *Segs, ULONG SegCount, ULONG Offset, PUCHAR Src, ULONG Length)
{
    ULONG i, pos = 0;

    for (i = 0; i < SegCount && Length; i++)
    {
        ULONG segLen = Segs[i].Length;

        if (pos + segLen <= Offset)
        {
            pos += segLen;
            continue;
        }
        {
            ULONG within = (Offset > pos) ? (Offset - pos) : 0;
            ULONG avail = segLen - within;
            ULONG take = min(avail, Length);

            RtlCopyMemory(Segs[i].Va + within, Src, take);
            Src += take;
            Length -= take;
            Offset += take;
            pos += segLen;
        }
    }
}

/*
 * Stage one direction into pool memory and emit its descriptors.
 *
 * A stream that fits in a control slot takes one; anything larger takes a run
 * of data chunks. `CopyIn` is TRUE for the device-readable direction, where
 * the bytes have to be there before the device looks.
 */
static BOOLEAN VirtFsStageStream(PRDMA_CLIENT c,
                                 VIRTFS_BOUNCE_SEG *Segs,
                                 ULONG SegCount,
                                 ULONG Total,
                                 BOOLEAN CopyIn,
                                 PVOID *Ctl,
                                 PVOID *Chunks,
                                 ULONG *ChunkLen,
                                 ULONG *ChunkCount,
                                 struct VirtIOBufferDescriptor *Sg,
                                 ULONG *SgCount)
{
    ULONG n = 0;
    ULONG off = 0;

    *Ctl = NULL;
    *ChunkCount = 0;
    *SgCount = 0;

    if (Total == 0)
    {
        return TRUE;
    }

    if (Total <= c->CtlSlotSize)
    {
        PVOID slot = RdmaClientAllocCtl(c);

        if (slot == NULL)
        {
            return FALSE;
        }
        if (CopyIn)
        {
            VirtFsCopyFromSegs(Segs, SegCount, 0, (PUCHAR)slot, Total);
        }
        *Ctl = slot;
        Sg[0].physAddr = RdmaClientVAtoPA(c, slot);
        Sg[0].length = Total;
        *SgCount = 1;
        return TRUE;
    }

    if (((Total + c->DataChunkSize - 1) / c->DataChunkSize) > VIRTFS_BOUNCE_MAX_CHUNKS)
    {
        /* Refuse rather than truncate: a short FUSE reply is silent corruption. */
        return FALSE;
    }

    while (off < Total)
    {
        PVOID chunk = RdmaClientAllocChunk(c);
        ULONG take;

        if (chunk == NULL)
        {
            /* Caller rolls back what we took: it owns the arrays. */
            *ChunkCount = n;
            *SgCount = n;
            return FALSE;
        }
        take = min(c->DataChunkSize, Total - off);
        if (CopyIn)
        {
            VirtFsCopyFromSegs(Segs, SegCount, off, (PUCHAR)chunk, take);
        }
        Chunks[n] = chunk;
        ChunkLen[n] = take;
        Sg[n].physAddr = RdmaClientVAtoPA(c, chunk);
        Sg[n].length = take;
        off += take;
        n++;
    }

    *ChunkCount = n;
    *SgCount = n;
    return TRUE;
}

static VOID VirtFsBounceFree(PRDMA_CLIENT c, PVIRTFS_BOUNCE b)
{
    ULONG i;

    for (i = 0; i < b->OutChunkCount; i++)
    {
        RdmaClientFreeChunk(c, b->OutChunks[i]);
    }
    for (i = 0; i < b->InChunkCount; i++)
    {
        RdmaClientFreeChunk(c, b->InChunks[i]);
    }
    if (b->OutCtl != NULL)
    {
        RdmaClientFreeCtl(c, b->OutCtl);
    }
    if (b->InCtl != NULL)
    {
        RdmaClientFreeCtl(c, b->InCtl);
    }
    RtlZeroMemory(b, sizeof(*b));
}

BOOLEAN VirtFsBounceBuild(PDEVICE_CONTEXT Context, PVIRTIO_FS_REQUEST Request, ULONG *OutNum, ULONG *InNum)
{
    PRDMA_CLIENT c = &Context->Rdma;
    PVIRTFS_BOUNCE b = &Request->Bounce;
    VIRTFS_BOUNCE_SEG outSegs[VIRTFS_BOUNCE_MAX_SEGS];
    ULONG outSegCount, outTotal = 0;
    ULONG outSg = 0, inSg = 0;

    RtlZeroMemory(b, sizeof(*b));

    outSegCount = VirtFsCollectSegs(&Request->H2D_Params, outSegs, VIRTFS_BOUNCE_MAX_SEGS, &outTotal);
    b->InSegCount = VirtFsCollectSegs(&Request->D2H_Params, b->InSegs, VIRTFS_BOUNCE_MAX_SEGS, &b->InTotalLength);

    if ((outTotal != 0 && outSegCount == 0) || (b->InTotalLength != 0 && b->InSegCount == 0))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "bounce: could not map the request buffers");
        return FALSE;
    }

    if (!VirtFsStageStream(c,
                           outSegs,
                           outSegCount,
                           outTotal,
                           TRUE,
                           &b->OutCtl,
                           b->OutChunks,
                           b->OutChunkLen,
                           &b->OutChunkCount,
                           Request->SGTable,
                           &outSg))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "bounce: no pool room for %u out bytes", outTotal);
        VirtFsBounceFree(c, b);
        return FALSE;
    }

    if (!VirtFsStageStream(c,
                           b->InSegs,
                           b->InSegCount,
                           b->InTotalLength,
                           FALSE,
                           &b->InCtl,
                           b->InChunks,
                           b->InChunkLen,
                           &b->InChunkCount,
                           Request->SGTable + outSg,
                           &inSg))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "bounce: no pool room for %u in bytes", b->InTotalLength);
        VirtFsBounceFree(c, b);
        return FALSE;
    }

    b->Staged = TRUE;
    *OutNum = outSg;
    *InNum = inSg;
    return TRUE;
}

VOID VirtFsBounceComplete(PDEVICE_CONTEXT Context, PVIRTIO_FS_REQUEST Request, ULONG Length)
{
    PRDMA_CLIENT c = &Context->Rdma;
    PVIRTFS_BOUNCE b = &Request->Bounce;
    ULONG i, off = 0;
    ULONG remaining;

    if (!b->Staged)
    {
        return;
    }

    /* Copy back only what the device says it wrote. Beyond that the staging
     * still holds whatever the previous user of the chunk left there. */
    remaining = min(Length, b->InTotalLength);

    if (b->InCtl != NULL)
    {
        VirtFsCopyToSegs(b->InSegs, b->InSegCount, 0, (PUCHAR)b->InCtl, remaining);
    }
    else
    {
        for (i = 0; i < b->InChunkCount && remaining; i++)
        {
            ULONG take = min(b->InChunkLen[i], remaining);

            VirtFsCopyToSegs(b->InSegs, b->InSegCount, off, (PUCHAR)b->InChunks[i], take);
            off += take;
            remaining -= take;
        }
    }

    VirtFsBounceFree(c, b);
}

VOID VirtFsBounceRelease(PDEVICE_CONTEXT Context, PVIRTIO_FS_REQUEST Request)
{
    /*
     * The Active test is not just an optimisation: device-context cleanup
     * drains whatever is still on the request list, and by then
     * VirtFsRdmaDisconnect has handed the whole region back to the pool
     * driver. Returning chunks to a client that no longer owns any memory
     * would be worse than leaking them, and there is nothing left to leak.
     *
     * VirtFsBounceComplete has no such guard on purpose: it must copy, and it
     * cannot run after the disconnect because VirtIOWdfShutdown has already
     * stopped the queues by then.
     */
    if (!Context->Rdma.Active)
    {
        RtlZeroMemory(&Request->Bounce, sizeof(Request->Bounce));
        return;
    }
    if (Request->Bounce.Staged || Request->Bounce.OutCtl || Request->Bounce.InCtl || Request->Bounce.OutChunkCount ||
        Request->Bounce.InChunkCount)
    {
        VirtFsBounceFree(&Context->Rdma, &Request->Bounce);
    }
}
