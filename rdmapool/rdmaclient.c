/*
 * rdmapool client library — shared by the Gunyah protected-VM storage
 * miniports (viostor, vioscsi). See rdmaclient.h for the design overview.
 *
 * Uses only WDM APIs (no StorPort), so it compiles unchanged into any
 * kernel-mode driver; all StorPort interaction stays in the caller.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>

#include "rdmaclient.h"

#include <wdmguid.h>
#include <initguid.h>
#include "rdmapool_interface.h"

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* ------------------------------------------------------------------ */
/* rdmapool connect / IOCTL                                           */
/* ------------------------------------------------------------------ */

static NTSTATUS RdmaClientIoctl(PRDMA_CLIENT c,
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
                                        c->PoolDeviceObject,
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
    irpStack->FileObject = c->PoolFileObject;

    status = IoCallDriver(c->PoolDeviceObject, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    return iosb.Status;
}

NTSTATUS RdmaClientConnect(PRDMA_CLIENT c, const char *Tag, ULONG RingPages, ULONG MetaPages)
{
    return RdmaClientConnectEx(c, Tag, RingPages, MetaPages, RDMA_CLIENT_DEFAULT_DATA_PAGES);
}

NTSTATUS RdmaClientConnectEx(PRDMA_CLIENT c, const char *Tag, ULONG RingPages, ULONG MetaPages, ULONG MaxDataPages)
{
    NTSTATUS status;
    PWSTR deviceInterfaceList = NULL;
    UNICODE_STRING deviceName;
    RDMAPOOL_QUERY_POOL_OUTPUT queryOutput;
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;
    ULONG totalPages, poolPages, bouncePages;

    c->Active = FALSE;
    c->Tag = Tag ? Tag : "rdmaclient";
    c->LastPoolTotalSize = 0;
    c->LastRequestedPages = 0;

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_RDMAPOOL, NULL, 0, &deviceInterfaceList);
    if (!NT_SUCCESS(status) || deviceInterfaceList == NULL || *deviceInterfaceList == L'\0')
    {
        DbgPrint("%s rdmapool: not found (0x%x), using normal DMA\n", c->Tag, status);
        if (deviceInterfaceList)
        {
            ExFreePool(deviceInterfaceList);
        }
        return STATUS_NOT_FOUND;
    }

    RtlInitUnicodeString(&deviceName, deviceInterfaceList);
    status = IoGetDeviceObjectPointer(&deviceName, FILE_ALL_ACCESS, &c->PoolFileObject, &c->PoolDeviceObject);
    ExFreePool(deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: IoGetDeviceObjectPointer failed 0x%x\n", c->Tag, status);
        c->PoolFileObject = NULL;
        c->PoolDeviceObject = NULL;
        return status;
    }

    RtlZeroMemory(&queryOutput, sizeof(queryOutput));
    status = RdmaClientIoctl(c, (ULONG)IOCTL_RDMAPOOL_QUERY_POOL, NULL, 0, &queryOutput, sizeof(queryOutput));
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: QUERY_POOL failed 0x%x\n", c->Tag, status);
        ObDereferenceObject(c->PoolFileObject);
        c->PoolFileObject = NULL;
        c->PoolDeviceObject = NULL;
        return status;
    }

    poolPages = (ULONG)(queryOutput.TotalSize / PAGE_SIZE);
    c->LastPoolTotalSize = queryOutput.TotalSize;

    /* Region = vrings + caller metadata (control slots / event area) + a data
     * area capped at MaxDataPages / half the pool (the pool is shared with the
     * other pVM drivers). */
    bouncePages = MetaPages + min(MaxDataPages, poolPages / 2);
    totalPages = RingPages + bouncePages;
    if (totalPages > poolPages)
    {
        totalPages = poolPages;
    }

    RtlZeroMemory(&allocInput, sizeof(allocInput));
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));
    allocInput.NumPages = totalPages;
    c->LastRequestedPages = totalPages;
    status = RdmaClientIoctl(c,
                             (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                             &allocInput,
                             sizeof(allocInput),
                             &allocOutput,
                             sizeof(allocOutput));
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: ALLOCATE %u pages failed 0x%x\n", c->Tag, totalPages, status);
        ObDereferenceObject(c->PoolFileObject);
        c->PoolFileObject = NULL;
        c->PoolDeviceObject = NULL;
        return status;
    }

    c->Active = TRUE;
    c->BaseVA = allocOutput.VirtualAddress;
    c->BasePA = allocOutput.PhysicalAddress;
    c->Size = (ULONG64)totalPages * PAGE_SIZE;

    DbgPrint("%s rdmapool: connected VA=%p PA=0x%I64x pages=%u (rings=%u bounce=%u)\n",
             c->Tag,
             c->BaseVA,
             c->BasePA.QuadPart,
             totalPages,
             RingPages,
             bouncePages);
    return STATUS_SUCCESS;
}

NTSTATUS RdmaClientOpen(PRDMA_CLIENT c, const char *Tag)
{
    NTSTATUS status;
    PWSTR deviceInterfaceList = NULL;
    UNICODE_STRING deviceName;
    RDMAPOOL_QUERY_POOL_OUTPUT queryOutput;

    c->Active = FALSE;
    c->Tag = Tag ? Tag : "rdmaclient";
    c->LastPoolTotalSize = 0;
    c->LastRequestedPages = 0;
    c->BaseVA = NULL;
    c->BasePA.QuadPart = 0;
    c->Size = 0;

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_RDMAPOOL, NULL, 0, &deviceInterfaceList);
    if (!NT_SUCCESS(status) || deviceInterfaceList == NULL || *deviceInterfaceList == L'\0')
    {
        DbgPrint("%s rdmapool: not found (0x%x), using normal DMA\n", c->Tag, status);
        if (deviceInterfaceList)
        {
            ExFreePool(deviceInterfaceList);
        }
        return STATUS_NOT_FOUND;
    }

    RtlInitUnicodeString(&deviceName, deviceInterfaceList);
    status = IoGetDeviceObjectPointer(&deviceName, FILE_ALL_ACCESS, &c->PoolFileObject, &c->PoolDeviceObject);
    ExFreePool(deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: IoGetDeviceObjectPointer failed 0x%x\n", c->Tag, status);
        c->PoolFileObject = NULL;
        c->PoolDeviceObject = NULL;
        return status;
    }

    RtlZeroMemory(&queryOutput, sizeof(queryOutput));
    status = RdmaClientIoctl(c, (ULONG)IOCTL_RDMAPOOL_QUERY_POOL, NULL, 0, &queryOutput, sizeof(queryOutput));
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: QUERY_POOL failed 0x%x\n", c->Tag, status);
        ObDereferenceObject(c->PoolFileObject);
        c->PoolFileObject = NULL;
        c->PoolDeviceObject = NULL;
        return status;
    }
    c->LastPoolTotalSize = queryOutput.TotalSize;
    return STATUS_SUCCESS;
}

NTSTATUS RdmaClientAllocRegion(PRDMA_CLIENT c, ULONG Pages, PVOID *Va, PPHYSICAL_ADDRESS Pa)
{
    NTSTATUS status;
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;

    *Va = NULL;
    Pa->QuadPart = 0;
    if (c->PoolDeviceObject == NULL || Pages == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&allocInput, sizeof(allocInput));
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));
    allocInput.NumPages = Pages;
    c->LastRequestedPages = Pages;
    status = RdmaClientIoctl(c,
                             (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                             &allocInput,
                             sizeof(allocInput),
                             &allocOutput,
                             sizeof(allocOutput));
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    *Va = allocOutput.VirtualAddress;
    *Pa = allocOutput.PhysicalAddress;
    return STATUS_SUCCESS;
}

VOID RdmaClientFreeRegion(PRDMA_CLIENT c, PVOID Va, ULONG Pages)
{
    RDMAPOOL_FREE_INPUT freeInput;

    if (c->PoolDeviceObject == NULL || Va == NULL || Pages == 0)
    {
        return;
    }
    RtlZeroMemory(&freeInput, sizeof(freeInput));
    freeInput.VirtualAddress = Va;
    freeInput.NumPages = Pages;
    (void)RdmaClientIoctl(c, (ULONG)IOCTL_RDMAPOOL_FREE, &freeInput, sizeof(freeInput), NULL, 0);
}

VOID RdmaClientClose(PRDMA_CLIENT c)
{
    if (c->PoolFileObject != NULL)
    {
        ObDereferenceObject(c->PoolFileObject);
        c->PoolFileObject = NULL;
    }
    c->PoolDeviceObject = NULL;
    c->Active = FALSE;
}

VOID RdmaClientDisconnect(PRDMA_CLIENT c)
{
    if (c->Active && c->BaseVA != NULL && c->PoolFileObject != NULL)
    {
        RDMAPOOL_FREE_INPUT freeInput;
        RtlZeroMemory(&freeInput, sizeof(freeInput));
        freeInput.VirtualAddress = c->BaseVA;
        freeInput.NumPages = (ULONG)(c->Size / PAGE_SIZE);
        (void)RdmaClientIoctl(c, (ULONG)IOCTL_RDMAPOOL_FREE, &freeInput, sizeof(freeInput), NULL, 0);
    }
    if (c->PoolFileObject != NULL)
    {
        ObDereferenceObject(c->PoolFileObject);
        c->PoolFileObject = NULL;
    }
    c->PoolDeviceObject = NULL;
    c->BaseVA = NULL;
    c->Size = 0;
    c->Active = FALSE;
}

/* ------------------------------------------------------------------ */
/* VA <-> PA (whole region is one contiguous rdmapool allocation)     */
/* ------------------------------------------------------------------ */

PHYSICAL_ADDRESS RdmaClientVAtoPA(PRDMA_CLIENT c, PVOID va)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = c->BasePA.QuadPart + ((ULONG_PTR)va - (ULONG_PTR)c->BaseVA);
    return pa;
}

PVOID RdmaClientPAtoVA(PRDMA_CLIENT c, PHYSICAL_ADDRESS pa)
{
    return (PVOID)((ULONG_PTR)c->BaseVA + (ULONG_PTR)(pa.QuadPart - c->BasePA.QuadPart));
}

BOOLEAN RdmaClientOwnsVA(PRDMA_CLIENT c, PVOID va)
{
    ULONG_PTR addr = (ULONG_PTR)va;
    ULONG_PTR base = (ULONG_PTR)c->BaseVA;
    return (c->Active && c->BaseVA != NULL && addr >= base && addr < base + c->Size);
}

/* ------------------------------------------------------------------ */
/* SLIST sub-allocator: control slots + contiguous data chunks         */
/* ------------------------------------------------------------------ */

PVOID RdmaClientAllocCtl(PRDMA_CLIENT c)
{
    return (PVOID)InterlockedPopEntrySList(&c->CtlFreeList);
}

VOID RdmaClientFreeCtl(PRDMA_CLIENT c, PVOID slot)
{
    InterlockedPushEntrySList(&c->CtlFreeList, (PSLIST_ENTRY)slot);
}

PVOID RdmaClientAllocChunk(PRDMA_CLIENT c)
{
    return (PVOID)InterlockedPopEntrySList(&c->DataFreeList);
}

VOID RdmaClientFreeChunk(PRDMA_CLIENT c, PVOID chunk)
{
    InterlockedPushEntrySList(&c->DataFreeList, (PSLIST_ENTRY)chunk);
}

NTSTATUS RdmaClientBounceInit(PRDMA_CLIENT c,
                              PVOID FreeStart,
                              ULONG CtlSlots,
                              ULONG CtlSlotSize,
                              ULONG EventBytes,
                              ULONG DataChunkSize)
{
    PUCHAR base;
    PUCHAR regionEnd;
    SIZE_T avail;
    ULONG chunk, i;
    SIZE_T ctlBytes, dataBytes, eventBytes;

    if (!c->Active)
    {
        return STATUS_NOT_SUPPORTED;
    }

    base = (PUCHAR)(((ULONG_PTR)FreeStart + PAGE_SIZE - 1) & ~((ULONG_PTR)PAGE_SIZE - 1));
    regionEnd = (PUCHAR)c->BaseVA + c->Size;
    if (base >= regionEnd)
    {
        DbgPrint("%s bounce: no room after rings\n", c->Tag);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    avail = (SIZE_T)(regionEnd - base);

    c->BounceInitialized = FALSE;
    InitializeSListHead(&c->CtlFreeList);
    InitializeSListHead(&c->DataFreeList);

    /* Optional event area (page-aligned so the slots that follow stay aligned). */
    c->EventBaseVA = NULL;
    c->EventBytes = EventBytes;
    if (EventBytes)
    {
        eventBytes = ((SIZE_T)EventBytes + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
        if (eventBytes >= avail)
        {
            DbgPrint("%s bounce: no room for event area\n", c->Tag);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        c->EventBaseVA = base;
        base += eventBytes;
        avail -= eventBytes;
    }

    /* Control slots: one per outstanding request, bounded by half the space. */
    ctlBytes = (SIZE_T)CtlSlots * CtlSlotSize;
    if (ctlBytes > avail / 2)
    {
        CtlSlots = (ULONG)((avail / 2) / CtlSlotSize);
        ctlBytes = (SIZE_T)CtlSlots * CtlSlotSize;
    }
    c->CtlBaseVA = base;
    c->CtlSlotSize = CtlSlotSize;
    c->CtlSlotCount = CtlSlots;
    for (i = 0; i < CtlSlots; i++)
    {
        RdmaClientFreeCtl(c, c->CtlBaseVA + (SIZE_T)i * CtlSlotSize);
    }

    /* Data chunks: large CONTIGUOUS blocks, each one descriptor; page-align and
     * shrink until there is at least one chunk per control slot (concurrency). */
    chunk = (DataChunkSize + PAGE_SIZE - 1) & ~((ULONG)PAGE_SIZE - 1);
    if (chunk == 0)
    {
        chunk = PAGE_SIZE;
    }
    c->DataBaseVA = c->CtlBaseVA + ctlBytes;
    dataBytes = avail - ctlBytes;
    while (chunk > PAGE_SIZE && (dataBytes / chunk) < (SIZE_T)CtlSlots)
    {
        chunk -= PAGE_SIZE;
    }
    c->DataChunkSize = chunk;
    c->DataChunkCount = (ULONG)(dataBytes / chunk);
    for (i = 0; i < c->DataChunkCount; i++)
    {
        RdmaClientFreeChunk(c, c->DataBaseVA + (SIZE_T)i * chunk);
    }

    c->BounceInitialized = TRUE;
    DbgPrint("%s bounce: evt=%uB ctl=%u(%uB) data=%u x %uKB @ VA=%p\n",
             c->Tag,
             c->EventBytes,
             c->CtlSlotCount,
             c->CtlSlotSize,
             c->DataChunkCount,
             c->DataChunkSize / 1024,
             base);
    return (c->CtlSlotCount && c->DataChunkCount) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

/* ------------------------------------------------------------------ */
/* Completion poll thread (~1ms cadence; idle = fully blocked)         */
/* ------------------------------------------------------------------ */

static VOID RdmaClientPollThreadRoutine(PVOID Context)
{
    PRDMA_CLIENT c = (PRDMA_CLIENT)Context;
    LARGE_INTEGER idleTick;

    /* Negative = relative, 100ns units. */
    idleTick.QuadPart = -(LONGLONG)(10 * 1000 * RDMA_CLIENT_POLL_IDLE_MS);

    for (;;)
    {
        if (InterlockedCompareExchange(&c->PollStop, 0, 0) != 0)
        {
            break;
        }

        if (c->BusyCb(c->CbContext))
        {
            /*
             * Busy: drain, then either sleep PollIntervalUs (gentle periodic
             * poll, default 1ms: reaps within ~1ms at low CPU instead of
             * stalling until the ~250ms StorPort watchdog when a completion
             * interrupt to an idle vCPU goes missing) or tight-spin when
             * PollIntervalUs==0 (max IOPS, pegs this thread's core only while
             * I/O is in flight). This thread runs at PASSIVE_LEVEL — DrainCb
             * releases its locks before returning — so KeDelayExecutionThread
             * is legal.
             */
            c->DrainCb(c->CbContext);
            if (c->PollIntervalUs == 0)
            {
                KeStallExecutionProcessor(RDMA_CLIENT_POLL_SPIN_US);
            }
            else
            {
                LARGE_INTEGER pollDelay;
                /* relative (negative), 100ns units: 1us = 10 * 100ns */
                pollDelay.QuadPart = -(LONGLONG)(10 * (LONGLONG)c->PollIntervalUs);
                KeDelayExecutionThread(KernelMode, FALSE, &pollDelay);
            }
        }
        else
        {
            /* Idle: block until a submit kicks us (safety-net timeout). ~0 CPU. */
            (void)KeWaitForSingleObject(&c->PollWake, Executive, KernelMode, FALSE, &idleTick);
            c->DrainCb(c->CbContext);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID RdmaClientPollKick(PRDMA_CLIENT c)
{
    if (c->PollThread)
    {
        KeSetEvent(&c->PollWake, IO_NO_INCREMENT, FALSE);
    }
}

NTSTATUS RdmaClientStartPoll(PRDMA_CLIENT c,
                             RDMA_CLIENT_BUSY_CB BusyCb,
                             RDMA_CLIENT_DRAIN_CB DrainCb,
                             PVOID CbContext,
                             ULONG PollIntervalUs)
{
    NTSTATUS status;
    HANDLE hThread = NULL;
    OBJECT_ATTRIBUTES oa;

    if (!c->Active)
    {
        return STATUS_NOT_SUPPORTED; /* poll thread only needed on the rdmapool path */
    }

    KeInitializeEvent(&c->PollWake, SynchronizationEvent, FALSE);
    c->PollStop = 0;
    c->PollThread = NULL;
    c->BusyCb = BusyCb;
    c->DrainCb = DrainCb;
    c->CbContext = CbContext;
    c->PollIntervalUs = PollIntervalUs;

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL, RdmaClientPollThreadRoutine, c);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s poll: PsCreateSystemThread failed 0x%x\n", c->Tag, status);
        return status;
    }

    status = ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &c->PollThread, NULL);
    ZwClose(hThread);
    if (!NT_SUCCESS(status))
    {
        /* Thread is running but we couldn't get a reference: ask it to stop. */
        InterlockedExchange(&c->PollStop, 1);
        KeSetEvent(&c->PollWake, IO_NO_INCREMENT, FALSE);
        c->PollThread = NULL;
        return status;
    }

    DbgPrint("%s poll: thread started (interval %uus, idle %ums safety net)\n",
             c->Tag,
             c->PollIntervalUs,
             RDMA_CLIENT_POLL_IDLE_MS);
    return STATUS_SUCCESS;
}

VOID RdmaClientStopPoll(PRDMA_CLIENT c)
{
    PVOID thread = c->PollThread;

    if (thread == NULL)
    {
        return;
    }
    InterlockedExchange(&c->PollStop, 1);
    KeSetEvent(&c->PollWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(thread);
    c->PollThread = NULL;
}
