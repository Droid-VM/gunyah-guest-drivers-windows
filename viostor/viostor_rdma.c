/*
 * viostor restricted-DMA-pool (rdmapool) support + bounce allocator + poll thread.
 * See viostor_rdma.h for the design overview.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "virtio_stor_hw_helper.h" /* brings storport, osdep, virtio_stor.h, srbhelper, SRB_* macros */
#include "virtio_stor_utils.h"

#include <wdmguid.h>
#include <initguid.h>
#include "rdmapool_interface.h"

/* ------------------------------------------------------------------ */
/* rdmapool connect / IOCTL                                           */
/* ------------------------------------------------------------------ */

static NTSTATUS VioStorRdmaPoolIoctl(PADAPTER_EXTENSION adaptExt,
                                     ULONG IoControlCode,
                                     PVOID InputBuffer,
                                     ULONG InputBufferLength,
                                     PVOID OutputBuffer,
                                     ULONG OutputBufferLength)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION irpStack;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        adaptExt->rdmaPoolDeviceObject,
                                        InputBuffer,
                                        InputBufferLength,
                                        OutputBuffer,
                                        OutputBufferLength,
                                        FALSE,
                                        &event,
                                        &iosb);
    if (irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->FileObject = adaptExt->rdmaPoolFileObject;

    status = IoCallDriver(adaptExt->rdmaPoolDeviceObject, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    return iosb.Status;
}

NTSTATUS VioStorConnectRdmaPool(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    NTSTATUS status;
    PWSTR deviceInterfaceList = NULL;
    UNICODE_STRING deviceName;
    RDMAPOOL_QUERY_POOL_OUTPUT queryOutput;
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;
    ULONG ringPages, bouncePages, totalPages, poolPages;

    adaptExt->rdmaPoolActive = FALSE;

    if (adaptExt->dump_mode)
    {
        return STATUS_NOT_SUPPORTED;
    }

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_RDMAPOOL, NULL, 0, &deviceInterfaceList);
    if (!NT_SUCCESS(status) || deviceInterfaceList == NULL || *deviceInterfaceList == L'\0')
    {
        DbgPrint(" rdmapool: not found (0x%x), using normal DMA\n", status);
        if (deviceInterfaceList)
        {
            ExFreePool(deviceInterfaceList);
        }
        return STATUS_NOT_FOUND;
    }

    RtlInitUnicodeString(&deviceName, deviceInterfaceList);
    status = IoGetDeviceObjectPointer(&deviceName,
                                      FILE_ALL_ACCESS,
                                      &adaptExt->rdmaPoolFileObject,
                                      &adaptExt->rdmaPoolDeviceObject);
    ExFreePool(deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(" rdmapool: IoGetDeviceObjectPointer failed 0x%x\n", status);
        adaptExt->rdmaPoolFileObject = NULL;
        adaptExt->rdmaPoolDeviceObject = NULL;
        return status;
    }

    RtlZeroMemory(&queryOutput, sizeof(queryOutput));
    status = VioStorRdmaPoolIoctl(adaptExt,
                                  (ULONG)IOCTL_RDMAPOOL_QUERY_POOL,
                                  NULL,
                                  0,
                                  &queryOutput,
                                  sizeof(queryOutput));
    if (!NT_SUCCESS(status))
    {
        DbgPrint(" rdmapool: QUERY_POOL failed 0x%x\n", status);
        ObDereferenceObject(adaptExt->rdmaPoolFileObject);
        adaptExt->rdmaPoolFileObject = NULL;
        adaptExt->rdmaPoolDeviceObject = NULL;
        return status;
    }

    poolPages = (ULONG)(queryOutput.TotalSize / PAGE_SIZE);

    /* Region = vrings (pageAllocationSize, already sized by FindAdapter) + bounce.
     * Bounce = queue_depth control slots + a data area capped at 32MB / half pool. */
    ringPages = adaptExt->pageAllocationSize / PAGE_SIZE;
    bouncePages = adaptExt->queue_depth * BOUNCE_CTL_PAGES;
    bouncePages += min(8192u, poolPages / 2);
    totalPages = ringPages + bouncePages;
    if (totalPages > poolPages)
    {
        totalPages = poolPages;
    }

    RtlZeroMemory(&allocInput, sizeof(allocInput));
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));
    allocInput.NumPages = totalPages;
    status = VioStorRdmaPoolIoctl(adaptExt,
                                  (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                                  &allocInput,
                                  sizeof(allocInput),
                                  &allocOutput,
                                  sizeof(allocOutput));
    if (!NT_SUCCESS(status))
    {
        DbgPrint(" rdmapool: ALLOCATE %u pages failed 0x%x\n", totalPages, status);
        ObDereferenceObject(adaptExt->rdmaPoolFileObject);
        adaptExt->rdmaPoolFileObject = NULL;
        adaptExt->rdmaPoolDeviceObject = NULL;
        return status;
    }

    adaptExt->rdmaPoolActive = TRUE;
    adaptExt->rdmaPoolBaseVA = allocOutput.VirtualAddress;
    adaptExt->rdmaPoolBasePA = allocOutput.PhysicalAddress;
    adaptExt->rdmaPoolSize = (ULONG64)totalPages * PAGE_SIZE;

    /* Redirect the page bump allocator (used by virtio_find_queues) at the pool. */
    adaptExt->pageAllocationVa = adaptExt->rdmaPoolBaseVA;
    adaptExt->pageAllocationSize = totalPages * PAGE_SIZE;
    adaptExt->pageOffset = 0;

    DbgPrint(" rdmapool: connected VA=%p PA=0x%I64x pages=%u (rings=%u bounce=%u)\n",
             adaptExt->rdmaPoolBaseVA,
             adaptExt->rdmaPoolBasePA.QuadPart,
             totalPages,
             ringPages,
             bouncePages);
    return STATUS_SUCCESS;
}

VOID VioStorDisconnectRdmaPool(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;

    if (adaptExt->rdmaPoolActive && adaptExt->rdmaPoolBaseVA != NULL && adaptExt->rdmaPoolFileObject != NULL)
    {
        RDMAPOOL_FREE_INPUT freeInput;
        RtlZeroMemory(&freeInput, sizeof(freeInput));
        freeInput.VirtualAddress = adaptExt->rdmaPoolBaseVA;
        freeInput.NumPages = (ULONG)(adaptExt->rdmaPoolSize / PAGE_SIZE);
        (void)VioStorRdmaPoolIoctl(adaptExt, (ULONG)IOCTL_RDMAPOOL_FREE, &freeInput, sizeof(freeInput), NULL, 0);
    }
    if (adaptExt->rdmaPoolFileObject != NULL)
    {
        ObDereferenceObject(adaptExt->rdmaPoolFileObject);
        adaptExt->rdmaPoolFileObject = NULL;
    }
    adaptExt->rdmaPoolDeviceObject = NULL;
    adaptExt->rdmaPoolBaseVA = NULL;
    adaptExt->rdmaPoolSize = 0;
    adaptExt->rdmaPoolActive = FALSE;
}

/* ------------------------------------------------------------------ */
/* VA <-> PA (whole region is one contiguous rdmapool allocation)     */
/* ------------------------------------------------------------------ */

PHYSICAL_ADDRESS VioStorRdmaVAtoPA(PVOID DeviceExtension, PVOID va)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = adaptExt->rdmaPoolBasePA.QuadPart + ((ULONG_PTR)va - (ULONG_PTR)adaptExt->rdmaPoolBaseVA);
    return pa;
}

PVOID VioStorRdmaPAtoVA(PVOID DeviceExtension, PHYSICAL_ADDRESS pa)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    return (PVOID)((ULONG_PTR)adaptExt->rdmaPoolBaseVA + (ULONG_PTR)(pa.QuadPart - adaptExt->rdmaPoolBasePA.QuadPart));
}

/* ------------------------------------------------------------------ */
/* SLIST sub-allocator: control slots + contiguous data chunks         */
/* ------------------------------------------------------------------ */

static PVOID BounceAllocCtl(PBOUNCE_ALLOCATOR a)
{
    return (PVOID)InterlockedPopEntrySList(&a->CtlFreeList);
}
static VOID BounceFreeCtl(PBOUNCE_ALLOCATOR a, PVOID p)
{
    InterlockedPushEntrySList(&a->CtlFreeList, (PSLIST_ENTRY)p);
}
static PVOID BounceAllocChunk(PBOUNCE_ALLOCATOR a)
{
    return (PVOID)InterlockedPopEntrySList(&a->DataFreeList);
}
static VOID BounceFreeChunk(PBOUNCE_ALLOCATOR a, PVOID p)
{
    InterlockedPushEntrySList(&a->DataFreeList, (PSLIST_ENTRY)p);
}

NTSTATUS VioStorBounceInit(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    PBOUNCE_ALLOCATOR a = &adaptExt->bounce;
    PUCHAR base;
    SIZE_T avail;
    ULONG chunk, ctlCount, i;
    SIZE_T ctlBytes, dataBytes;

    if (!adaptExt->rdmaPoolActive)
    {
        return STATUS_NOT_SUPPORTED;
    }

    /* Bounce region = whatever rdmapool space is left after the vrings. */
    base = (PUCHAR)adaptExt->pageAllocationVa + adaptExt->pageOffset;
    base = (PUCHAR)(((ULONG_PTR)base + PAGE_SIZE - 1) & ~((ULONG_PTR)PAGE_SIZE - 1));
    if ((ULONG_PTR)base >= (ULONG_PTR)adaptExt->rdmaPoolBaseVA + adaptExt->pageAllocationSize)
    {
        DbgPrint(" bounce: no room after rings\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    avail = (SIZE_T)((ULONG_PTR)adaptExt->rdmaPoolBaseVA + adaptExt->pageAllocationSize - (ULONG_PTR)base);

    RtlZeroMemory(a, sizeof(*a));
    InitializeSListHead(&a->CtlFreeList);
    InitializeSListHead(&a->DataFreeList);
    a->BaseVA = base;
    a->BasePA = VioStorRdmaVAtoPA(DeviceExtension, base);

    /* Control slots: one per outstanding request (queue_depth), bounded by space. */
    ctlCount = adaptExt->queue_depth ? adaptExt->queue_depth : 64;
    ctlBytes = (SIZE_T)ctlCount * BOUNCE_CTL_SIZE;
    if (ctlBytes > avail / 2)
    {
        ctlCount = (ULONG)((avail / 2) / BOUNCE_CTL_SIZE);
        ctlBytes = (SIZE_T)ctlCount * BOUNCE_CTL_SIZE;
    }
    a->CtlBaseVA = base;
    a->CtlSlotCount = ctlCount;
    for (i = 0; i < ctlCount; i++)
    {
        BounceFreeCtl(a, a->CtlBaseVA + (SIZE_T)i * BOUNCE_CTL_SIZE);
    }

    /* Data chunks: large CONTIGUOUS blocks, each <= device size_max so a chunk is
     * one descriptor. Default 256KB; clamp to size_max; page-align. */
    chunk = BOUNCE_DATA_CHUNK_SIZE;
    /* Only clamp to size_max when the device actually advertises a segment limit;
     * info.size_max defaults to PAGE_SIZE when the feature is absent, which would
     * needlessly shatter a contiguous chunk into per-page descriptors. */
    if (CHECKBIT(adaptExt->features, VIRTIO_BLK_F_SIZE_MAX) && adaptExt->info.size_max &&
        adaptExt->info.size_max < chunk)
    {
        chunk = adaptExt->info.size_max;
    }
    chunk = (chunk + PAGE_SIZE - 1) & ~((ULONG)PAGE_SIZE - 1);
    if (chunk == 0)
    {
        chunk = PAGE_SIZE;
    }

    a->DataBaseVA = a->CtlBaseVA + ctlBytes;
    dataBytes = avail - ctlBytes;
    /* Shrink chunk if we can't make at least queue_depth chunks (keep concurrency). */
    while (chunk > PAGE_SIZE && (dataBytes / chunk) < (SIZE_T)ctlCount)
    {
        chunk -= PAGE_SIZE;
    }
    a->DataChunkSize = chunk;
    a->DataChunkCount = (ULONG)(dataBytes / chunk);
    for (i = 0; i < a->DataChunkCount; i++)
    {
        BounceFreeChunk(a, a->DataBaseVA + (SIZE_T)i * chunk);
    }

    a->Initialized = TRUE;
    DbgPrint(" bounce: ctl=%u(%uB) data=%u x %uKB @ VA=%p\n",
             a->CtlSlotCount,
             BOUNCE_CTL_SIZE,
             a->DataChunkCount,
             a->DataChunkSize / 1024,
             a->BaseVA);
    return (a->CtlSlotCount && a->DataChunkCount) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

/* ------------------------------------------------------------------ */
/* Bounce build / complete                                            */
/* ------------------------------------------------------------------ */

PVOID VioStorBounceAllocCtl(PVOID DeviceExtension, PVOID srbExtArg)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    PSRB_EXTENSION srbExt = (PSRB_EXTENSION)srbExtArg;
    PVOID ctl;

    if (!adaptExt->rdmaPoolActive || !adaptExt->bounce.Initialized)
    {
        return NULL;
    }
    ctl = BounceAllocCtl(&adaptExt->bounce);
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
    PBOUNCE_ALLOCATOR a = &adaptExt->bounce;
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
            BounceFreeCtl(a, ctl);
            srbExt->bounceCtl = NULL;
            return FALSE;
        }
    }
    srbExt->srbDataVA = (PUCHAR)dataVA;
    srbExt->srbDataLen = dataLen;

    nChunks = dataLen ? ((dataLen + a->DataChunkSize - 1) / a->DataChunkSize) : 0;

    /* sg[0] = out_hdr (ctl), sg[1..nChunks] = data chunks, sg[last] = status (ctl). */
    srbExt->sg[0].physAddr = VioStorRdmaVAtoPA(DeviceExtension, (PUCHAR)ctl + BOUNCE_CTL_OUTHDR_OFFSET);
    srbExt->sg[0].length = sizeof(srbExt->vbr.out_hdr);

    off = 0;
    for (sgIdx = 1; sgIdx <= nChunks; sgIdx++)
    {
        PVOID chunk = BounceAllocChunk(a);
        ULONG clen;
        if (chunk == NULL)
        {
            /* Roll back chunks already taken, then the ctl slot. */
            ULONG k;
            for (k = 1; k < sgIdx; k++)
            {
                BounceFreeChunk(a, VioStorRdmaPAtoVA(DeviceExtension, srbExt->sg[k].physAddr));
            }
            BounceFreeCtl(a, ctl);
            srbExt->bounceCtl = NULL;
            srbExt->bounceChunkCount = 0;
            DbgPrint(" bounce: no data chunk\n");
            return FALSE;
        }
        clen = min(a->DataChunkSize, dataLen - off);
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
    PBOUNCE_ALLOCATOR a = &adaptExt->bounce;
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
        BounceFreeChunk(a, VioStorRdmaPAtoVA(DeviceExtension, srbExt->sg[i].physAddr));
    }
    BounceFreeCtl(a, ctl);
    srbExt->bounceCtl = NULL;
    srbExt->bounceChunkCount = 0;
}

/* ------------------------------------------------------------------ */
/* Completion poll thread (~1ms cadence; idle = fully blocked)         */
/* ------------------------------------------------------------------ */

static BOOLEAN VioStorAnyOutstanding(PADAPTER_EXTENSION adaptExt)
{
    ULONG q;
    for (q = 0; q < adaptExt->num_queues; q++)
    {
        if (adaptExt->processing_srbs[q].srb_cnt != 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID VioStorPollThreadRoutine(PVOID Context)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)Context;
    LARGE_INTEGER idleTick;

    /* Negative = relative, 100ns units. */
    idleTick.QuadPart = -(LONGLONG)(10 * 1000 * VIOSTOR_POLL_IDLE_MS);

    for (;;)
    {
        ULONG q;

        if (InterlockedCompareExchange(&adaptExt->pollStop, 0, 0) != 0)
        {
            break;
        }

        if (VioStorAnyOutstanding(adaptExt))
        {
            /*
             * Busy: tight spin-drain for low latency. Burns this dedicated thread's
             * CPU only while I/O is in flight and never blocks StartIo, so queue
             * depth is preserved (unlike the old inline busy-poll). The short stall
             * between drains releases the queue lock so submits/ISR make progress.
             * (Uncached small random I/O is per-request-rate bound ~11k IOPS — the
             * single-queue bounce round-trip floor, not lock contention; cached 4K
             * random reaches ~78k IOPS / 307 MB/s.)
             */
            for (q = 0; q < adaptExt->num_queues; q++)
            {
                VioStorCompleteRequest(adaptExt, q + adaptExt->msix_has_config_vector, FALSE);
            }
            /*
             * Gentle periodic poll (default): SLEEP pollIntervalUs between drains so
             * this thread yields its CPU instead of pegging a core with a tight
             * KeStallExecutionProcessor busy-spin. 1ms keeps completion latency low
             * (reaped within ~1ms, vs the ~250ms StorPort watchdog that capped INTx
             * at ~5MB/s) without the busy-spin power cost. The thread runs at
             * PASSIVE_LEVEL (VioStorCompleteRequest releases its lock before we return
             * here), so KeDelayExecutionThread is legal. PollIntervalUs=0 restores the
             * tight spin for max IOPS.
             */
            if (adaptExt->pollIntervalUs == 0)
            {
                KeStallExecutionProcessor(VIOSTOR_POLL_SPIN_US);
            }
            else
            {
                LARGE_INTEGER pollDelay;
                /* relative (negative), 100ns units: 1us = 10 * 100ns */
                pollDelay.QuadPart = -(LONGLONG)(10 * (LONGLONG)adaptExt->pollIntervalUs);
                KeDelayExecutionThread(KernelMode, FALSE, &pollDelay);
            }
        }
        else
        {
            /* Idle: block until a submit kicks us (safety-net timeout). ~0 CPU. */
            (void)KeWaitForSingleObject(&adaptExt->pollWake, Executive, KernelMode, FALSE, &idleTick);
            for (q = 0; q < adaptExt->num_queues; q++)
            {
                VioStorCompleteRequest(adaptExt, q + adaptExt->msix_has_config_vector, FALSE);
            }
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID VioStorPollKick(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    if (adaptExt->pollThread)
    {
        KeSetEvent(&adaptExt->pollWake, IO_NO_INCREMENT, FALSE);
    }
}

NTSTATUS VioStorStartPollThread(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    NTSTATUS status;
    HANDLE hThread = NULL;
    OBJECT_ATTRIBUTES oa;

    if (!adaptExt->rdmaPoolActive)
    {
        return STATUS_NOT_SUPPORTED; /* poll thread only needed on the rdmapool path */
    }

    KeInitializeEvent(&adaptExt->pollWake, SynchronizationEvent, FALSE);
    adaptExt->pollStop = 0;
    adaptExt->pollThread = NULL;

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL, VioStorPollThreadRoutine, adaptExt);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(" poll: PsCreateSystemThread failed 0x%x\n", status);
        return status;
    }

    status = ObReferenceObjectByHandle(hThread,
                                       THREAD_ALL_ACCESS,
                                       *PsThreadType,
                                       KernelMode,
                                       &adaptExt->pollThread,
                                       NULL);
    ZwClose(hThread);
    if (!NT_SUCCESS(status))
    {
        /* Thread is running but we couldn't get a reference: ask it to stop. */
        InterlockedExchange(&adaptExt->pollStop, 1);
        KeSetEvent(&adaptExt->pollWake, IO_NO_INCREMENT, FALSE);
        adaptExt->pollThread = NULL;
        return status;
    }

    DbgPrint(" poll: thread started (adaptive: spin %uus busy / %ums idle)\n",
             VIOSTOR_POLL_SPIN_US,
             VIOSTOR_POLL_IDLE_MS);
    return STATUS_SUCCESS;
}

VOID VioStorStopPollThread(PVOID DeviceExtension)
{
    PADAPTER_EXTENSION adaptExt = (PADAPTER_EXTENSION)DeviceExtension;
    PVOID thread = adaptExt->pollThread;

    if (thread == NULL)
    {
        return;
    }
    InterlockedExchange(&adaptExt->pollStop, 1);
    KeSetEvent(&adaptExt->pollWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(thread);
    adaptExt->pollThread = NULL;
}
