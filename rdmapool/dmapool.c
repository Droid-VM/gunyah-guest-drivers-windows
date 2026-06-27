/*
 * Restricted DMA Pool - Bitmap Page Allocator Implementation
 *
 * Manages a fixed physical memory region using a bitmap allocator.
 * Each bit represents one PAGE_SIZE page. Set = allocated.
 *
 * Thread-safe: protected by a spinlock.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>
#include "dmapool.h"

#define RDMAPOOL_TAG 'PMDR'

static PHYSICAL_ADDRESS gPoolPhysBase;
static PVOID gPoolVirtBase;
static ULONG gPoolTotalPages;
static SIZE_T gPoolTotalSize;

static PUCHAR gBitmap;
static ULONG gBitmapBytes;
static KSPIN_LOCK gPoolLock;

static __forceinline BOOLEAN BitmapTestBit(ULONG Index)
{
    return (gBitmap[Index / 8] & (1U << (Index % 8))) != 0;
}

static __forceinline VOID BitmapSetBit(ULONG Index)
{
    gBitmap[Index / 8] |= (UCHAR)(1U << (Index % 8));
}

static __forceinline VOID BitmapClearBit(ULONG Index)
{
    gBitmap[Index / 8] &= (UCHAR) ~(1U << (Index % 8));
}

/*
 * Find a contiguous run of free pages in the bitmap.
 */
static BOOLEAN BitmapFindFreeRun(_In_ ULONG NumPages, _Out_ ULONG *StartPage)
{
    ULONG RunStart = 0;
    ULONG RunLen = 0;
    ULONG i;

    for (i = 0; i < gPoolTotalPages; i++)
    {
        if (BitmapTestBit(i))
        {
            RunLen = 0;
            RunStart = i + 1;
        }
        else
        {
            RunLen++;
            if (RunLen >= NumPages)
            {
                *StartPage = RunStart;
                return TRUE;
            }
        }
    }

    return FALSE;
}

NTSTATUS
DmaPoolInit(_In_ PHYSICAL_ADDRESS PhysicalBase, _In_ PVOID VirtualBase, _In_ SIZE_T TotalSize)
{
    gPoolPhysBase = PhysicalBase;
    gPoolVirtBase = VirtualBase;
    gPoolTotalSize = TotalSize;
    gPoolTotalPages = (ULONG)(TotalSize / PAGE_SIZE);

    gBitmapBytes = (gPoolTotalPages + 7) / 8;
    gBitmap = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, gBitmapBytes, RDMAPOOL_TAG);
    if (gBitmap == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(gBitmap, gBitmapBytes);
    KeInitializeSpinLock(&gPoolLock);

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "rdmapool: DmaPoolInit base PA=0x%llx VA=%p size=0x%llx (%u pages)\n",
               PhysicalBase.QuadPart,
               VirtualBase,
               (ULONG64)TotalSize,
               gPoolTotalPages);

    return STATUS_SUCCESS;
}

VOID DmaPoolDestroy(VOID)
{
    if (gBitmap != NULL)
    {
        ExFreePoolWithTag(gBitmap, RDMAPOOL_TAG);
        gBitmap = NULL;
    }
    gPoolVirtBase = NULL;
    gPoolTotalPages = 0;
}

NTSTATUS
DmaPoolAllocatePages(_In_ ULONG NumPages, _Out_ PVOID *VirtualAddress, _Out_ PHYSICAL_ADDRESS *PhysicalAddress)
{
    KIRQL OldIrql;
    ULONG StartPage;
    ULONG i;

    *VirtualAddress = NULL;
    PhysicalAddress->QuadPart = 0;

    if (NumPages == 0 || gBitmap == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&gPoolLock, &OldIrql);

    if (!BitmapFindFreeRun(NumPages, &StartPage))
    {
        KeReleaseSpinLock(&gPoolLock, OldIrql);
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: pool exhausted (requested %u pages, total %u)\n",
                   NumPages,
                   gPoolTotalPages);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = StartPage; i < StartPage + NumPages; i++)
    {
        BitmapSetBit(i);
    }

    KeReleaseSpinLock(&gPoolLock, OldIrql);

    *VirtualAddress = (PUCHAR)gPoolVirtBase + ((SIZE_T)StartPage * PAGE_SIZE);
    PhysicalAddress->QuadPart = gPoolPhysBase.QuadPart + ((LONGLONG)StartPage * PAGE_SIZE);

    RtlZeroMemory(*VirtualAddress, (SIZE_T)NumPages * PAGE_SIZE);

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_TRACE_LEVEL,
               "rdmapool: allocated %u pages @ VA=%p PA=0x%llx\n",
               NumPages,
               *VirtualAddress,
               PhysicalAddress->QuadPart);

    return STATUS_SUCCESS;
}

VOID DmaPoolFreePages(_In_ PVOID VirtualAddress, _In_ ULONG NumPages)
{
    KIRQL OldIrql;
    ULONG_PTR Offset;
    ULONG StartPage;
    ULONG i;

    if (VirtualAddress == NULL || gBitmap == NULL || gPoolVirtBase == NULL)
    {
        return;
    }

    if ((ULONG_PTR)VirtualAddress < (ULONG_PTR)gPoolVirtBase)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: free VA=%p below pool base %p\n",
                   VirtualAddress,
                   gPoolVirtBase);
        return;
    }

    Offset = (ULONG_PTR)VirtualAddress - (ULONG_PTR)gPoolVirtBase;
    StartPage = (ULONG)(Offset / PAGE_SIZE);

    if ((StartPage + NumPages) > gPoolTotalPages)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: free out of range VA=%p pages=%u\n",
                   VirtualAddress,
                   NumPages);
        return;
    }

    KeAcquireSpinLock(&gPoolLock, &OldIrql);

    for (i = StartPage; i < StartPage + NumPages; i++)
    {
        BitmapClearBit(i);
    }

    KeReleaseSpinLock(&gPoolLock, OldIrql);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_TRACE_LEVEL, "rdmapool: freed %u pages @ VA=%p\n", NumPages, VirtualAddress);
}

VOID DmaPoolQueryInfo(_Out_ PVOID *BaseVirtualAddress,
                      _Out_ PHYSICAL_ADDRESS *BasePhysicalAddress,
                      _Out_ ULONG64 *TotalSize)
{
    *BaseVirtualAddress = gPoolVirtBase;
    *BasePhysicalAddress = gPoolPhysBase;
    *TotalSize = (ULONG64)gPoolTotalSize;
}

NTSTATUS
DmaPoolReservePages(_In_ ULONG NumPages)
{
    KIRQL OldIrql;
    ULONG i;

    if (NumPages == 0 || gBitmap == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (NumPages > gPoolTotalPages)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: reserve %u pages exceeds pool (%u pages)\n",
                   NumPages,
                   gPoolTotalPages);
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&gPoolLock, &OldIrql);

    for (i = 0; i < NumPages; i++)
    {
        BitmapSetBit(i);
    }

    KeReleaseSpinLock(&gPoolLock, OldIrql);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "rdmapool: reserved %u pages from pool start\n", NumPages);

    return STATUS_SUCCESS;
}
