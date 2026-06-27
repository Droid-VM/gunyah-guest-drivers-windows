/*
 * Implementation of virtio_system_ops VirtioLib callbacks
 *
 * Copyright (c) 2016-2017 Red Hat, Inc.
 *
 * Author(s):
 *  Yuri Benditovich <ybendito@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIOWdf.h"
#include "private.h"
#include <devpropdef.h>
#include "rdmapool_interface.h"

static EVT_WDF_OBJECT_CONTEXT_DESTROY OnDmaTransactionDestroy;
static EVT_WDF_PROGRAM_DMA OnDmaTransactionProgramDma;

/* Helper: check if a VA falls within the rdmapool region */
static BOOLEAN IsRdmaPoolAddress(PVIRTIO_WDF_DRIVER pWdfDriver, PVOID va)
{
    if (!pWdfDriver->RdmaPoolActive || pWdfDriver->RdmaPoolBaseVA == NULL) {
        return FALSE;
    }
    ULONG_PTR addr = (ULONG_PTR)va;
    ULONG_PTR base = (ULONG_PTR)pWdfDriver->RdmaPoolBaseVA;
    return (addr >= base && addr < base + pWdfDriver->RdmaPoolSize);
}

/* Tracking entry for rdmapool allocations */
typedef struct _RDMAPOOL_ALLOC_ENTRY {
    LIST_ENTRY ListEntry;
    PVOID VirtualAddress;
    ULONG NumPages;
} RDMAPOOL_ALLOC_ENTRY, *PRDMAPOOL_ALLOC_ENTRY;

#define RDMAPOOL_ALLOC_TAG 'ARDR'

/* Allocate DMA memory from rdmapool via IOCTL */
static void *AllocateFromRdmaPool(PVIRTIO_WDF_DRIVER pWdfDriver, size_t size)
{
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;
    PIO_STACK_LOCATION irpStack;

    allocInput.NumPages = (ULONG)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(IOCTL_RDMAPOOL_ALLOCATE, pWdfDriver->RdmaPoolDeviceObject,
                                        &allocInput, sizeof(allocInput), &allocOutput,
                                        sizeof(allocOutput), FALSE, &event, &iosb);

    if (irp == NULL) {
        DPrintf(0, "%s: IoBuildDeviceIoControlRequest failed\n", __FUNCTION__);
        return NULL;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->FileObject = pWdfDriver->RdmaPoolFileObject;

    status = IoCallDriver(pWdfDriver->RdmaPoolDeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s: IOCTL_RDMAPOOL_ALLOCATE failed 0x%x (size=0x%x)\n", __FUNCTION__, status,
                (ULONG)size);
        return NULL;
    }

    /* Track this allocation for later free */
    {
        PRDMAPOOL_ALLOC_ENTRY entry = (PRDMAPOOL_ALLOC_ENTRY)ExAllocatePoolUninitialized(
            NonPagedPool, sizeof(RDMAPOOL_ALLOC_ENTRY), RDMAPOOL_ALLOC_TAG);
        if (entry != NULL) {
            RtlZeroMemory(entry, sizeof(*entry));
            entry->VirtualAddress = allocOutput.VirtualAddress;
            entry->NumPages = allocInput.NumPages;
            WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
            InsertTailList(&pWdfDriver->RdmaPoolAllocList, &entry->ListEntry);
            WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
        }
    }

    DPrintf(1, "%s: rdmapool alloc VA=%p PA=0x%llx size=0x%x\n", __FUNCTION__,
            allocOutput.VirtualAddress, allocOutput.PhysicalAddress.QuadPart, (ULONG)size);

    return allocOutput.VirtualAddress;
}

/* Free DMA memory back to rdmapool via IOCTL */
static void FreeToRdmaPool(PVIRTIO_WDF_DRIVER pWdfDriver, void *va, size_t size)
{
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    RDMAPOOL_FREE_INPUT freeInput;
    PIO_STACK_LOCATION irpStack;

    freeInput.VirtualAddress = va;
    freeInput.NumPages = (ULONG)((size + PAGE_SIZE - 1) / PAGE_SIZE);

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp =
        IoBuildDeviceIoControlRequest(IOCTL_RDMAPOOL_FREE, pWdfDriver->RdmaPoolDeviceObject,
                                      &freeInput, sizeof(freeInput), NULL, 0, FALSE, &event, &iosb);

    if (irp == NULL) {
        DPrintf(0, "%s: IoBuildDeviceIoControlRequest failed\n", __FUNCTION__);
        return;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->FileObject = pWdfDriver->RdmaPoolFileObject;

    status = IoCallDriver(pWdfDriver->RdmaPoolDeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s: IOCTL_RDMAPOOL_FREE failed 0x%x\n", __FUNCTION__, status);
    }
}

static void *AllocateCommonBuffer(PVIRTIO_WDF_DRIVER pWdfDriver, size_t size, ULONG groupTag)
{
    NTSTATUS status;
    WDFCOMMONBUFFER commonBuffer;
    PVIRTIO_WDF_MEMORY_BLOCK_CONTEXT context;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VIRTIO_WDF_MEMORY_BLOCK_CONTEXT);

    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        DPrintf(0, "%s FAILED(irql)\n", __FUNCTION__);
        return NULL;
    }
    status = WdfCommonBufferCreate(pWdfDriver->DmaEnabler, size, &attr, &commonBuffer);
    if (!NT_SUCCESS(status)) {
        return NULL;
    }
    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    status = WdfCollectionAdd(pWdfDriver->MemoryBlockCollection, commonBuffer);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(commonBuffer);
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
        return NULL;
    }
    context = GetMemoryBlockContext(commonBuffer);
    context->WdfBuffer = commonBuffer;
    context->Length = size;
    context->PhysicalAddress = WdfCommonBufferGetAlignedLogicalAddress(commonBuffer);
    context->pVirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(commonBuffer);
    context->groupTag = groupTag;
    context->bToBeDeleted = FALSE;
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    RtlZeroMemory(context->pVirtualAddress, size);

    DPrintf(1, "%s done %p@%I64x(tag %08X), size 0x%x\n", __FUNCTION__, context->pVirtualAddress,
            context->PhysicalAddress.QuadPart, context->groupTag, (ULONG)size);

    return context->pVirtualAddress;
}

void *VirtIOWdfDeviceAllocDmaMemory(VirtIODevice *vdev, size_t size, ULONG groupTag)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;

    /* If restricted DMA pool is active, allocate from it */
    if (pWdfDriver->RdmaPoolActive) {
        return AllocateFromRdmaPool(pWdfDriver, size);
    }

    return AllocateCommonBuffer(pWdfDriver, size, groupTag);
}

static BOOLEAN FindCommonBuffer(PVIRTIO_WDF_DRIVER pWdfDriver, void *p, PHYSICAL_ADDRESS *ppa,
                                size_t *pOffset, BOOLEAN bRemoval)
{
    BOOLEAN b = FALSE;
    ULONG_PTR va = (ULONG_PTR)p;
    ULONG i, n;
    WDFOBJECT obj = NULL;
    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    n = WdfCollectionGetCount(pWdfDriver->MemoryBlockCollection);
    for (i = 0; i < n; ++i) {
        obj = WdfCollectionGetItem(pWdfDriver->MemoryBlockCollection, i);
        if (!obj) {
            break;
        }
        PVIRTIO_WDF_MEMORY_BLOCK_CONTEXT context = GetMemoryBlockContext(obj);
        if (context->bToBeDeleted && !bRemoval) {
            continue;
        }
        ULONG_PTR currentVaStart = (ULONG_PTR)context->pVirtualAddress;
        if (va >= currentVaStart && va < (currentVaStart + context->Length)) {
            *ppa = context->PhysicalAddress;
            *pOffset = va - currentVaStart;
            b = TRUE;
            if (bRemoval) {
                b = *pOffset == 0;
                if (b) {
                    context->bToBeDeleted = TRUE;
                }
            }
            break;
        }
    }
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    if (!b) {
        DPrintf(0, "%s(%s) FAILED!\n", __FUNCTION__, bRemoval ? "Remove" : "Locate");
    } else if (bRemoval) {
        if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
            WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
            WdfCollectionRemove(pWdfDriver->MemoryBlockCollection, obj);
            WdfSpinLockRelease(pWdfDriver->DmaSpinlock);

            WdfObjectDelete(obj);
            DPrintf(1, "%s %p freed (%d common buffers)\n", __FUNCTION__, va, n - 1);
        } else {
            DPrintf(0, "%s %p marked for deletion\n", __FUNCTION__, va);
        }
    }
    return b;
}

static PHYSICAL_ADDRESS GetPhysicalAddress(PVIRTIO_WDF_DRIVER pWdfDriver, PVOID va)
{
    PHYSICAL_ADDRESS pa;
    size_t offset;
    pa.QuadPart = 0;
    if (FindCommonBuffer(pWdfDriver, va, &pa, &offset, FALSE)) {
        pa.QuadPart += offset;
    }
    return pa;
}

PHYSICAL_ADDRESS VirtIOWdfDeviceGetPhysicalAddress(VirtIODevice *vdev, void *va)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;

    /* If the VA is within the rdmapool region, compute PA directly */
    if (IsRdmaPoolAddress(pWdfDriver, va)) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = pWdfDriver->RdmaPoolBasePA.QuadPart +
                      ((ULONG_PTR)va - (ULONG_PTR)pWdfDriver->RdmaPoolBaseVA);
        return pa;
    }

    return GetPhysicalAddress(pWdfDriver, va);
}

void VirtIOWdfDeviceFreeDmaMemory(VirtIODevice *vdev, void *va)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;

    /* If the VA is within the rdmapool region, free it there */
    if (IsRdmaPoolAddress(pWdfDriver, va)) {
        PLIST_ENTRY entry;
        PRDMAPOOL_ALLOC_ENTRY allocEntry = NULL;
        ULONG numPages = 0;

        /* Find and remove the tracking entry */
        WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
        for (entry = pWdfDriver->RdmaPoolAllocList.Flink; entry != &pWdfDriver->RdmaPoolAllocList;
             entry = entry->Flink) {
            PRDMAPOOL_ALLOC_ENTRY candidate =
                CONTAINING_RECORD(entry, RDMAPOOL_ALLOC_ENTRY, ListEntry);
            if (candidate->VirtualAddress == va) {
                allocEntry = candidate;
                numPages = candidate->NumPages;
                RemoveEntryList(entry);
                break;
            }
        }
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);

        if (allocEntry != NULL) {
            FreeToRdmaPool(pWdfDriver, va, (size_t)numPages * PAGE_SIZE);
            ExFreePoolWithTag(allocEntry, RDMAPOOL_ALLOC_TAG);
            DPrintf(1, "%s: freed rdmapool VA=%p pages=%u\n", __FUNCTION__, va, numPages);
        } else {
            DPrintf(0, "%s: rdmapool VA=%p not found in tracking list\n", __FUNCTION__, va);
        }
        return;
    }

    PHYSICAL_ADDRESS pa;
    size_t offset;
    FindCommonBuffer(pWdfDriver, va, &pa, &offset, TRUE);
}

static BOOLEAN FindCommonBufferByTag(PVIRTIO_WDF_DRIVER pWdfDriver, ULONG tag)
{
    BOOLEAN b = FALSE;
    ULONG i, n;
    WDFOBJECT obj = NULL;
    PVIRTIO_WDF_MEMORY_BLOCK_CONTEXT context = NULL;
    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    n = WdfCollectionGetCount(pWdfDriver->MemoryBlockCollection);
    for (i = 0; i < n; ++i) {
        obj = WdfCollectionGetItem(pWdfDriver->MemoryBlockCollection, i);
        if (!obj) {
            break;
        }
        context = GetMemoryBlockContext(obj);
        if (context->groupTag == tag) {
            b = TRUE;
            break;
        }
    }
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    if (b) {
        DPrintf(1, "%s %p (tag %08X) freed (%d common buffers)\n", __FUNCTION__,
                context->pVirtualAddress, tag, n - 1);
        WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
        WdfCollectionRemove(pWdfDriver->MemoryBlockCollection, obj);
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
        WdfObjectDelete(obj);
    }
    return b;
}

void VirtIOWdfDeviceFreeDmaMemoryByTag(VirtIODevice *vdev, ULONG groupTag)
{
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        DPrintf(0, "%s FAILED(irql)\n", __FUNCTION__);
        return;
    }
    if (!groupTag) {
        DPrintf(0, "%s FAILED(default tag)\n", __FUNCTION__);
        return;
    }
    if (!vdev->DeviceContext) {
        DPrintf(0, "%s was not initialized\n", __FUNCTION__);
        return;
    }
    while (FindCommonBufferByTag(vdev->DeviceContext, groupTag))
        ;
}

static void FreeSlicedBlock(PVIRTIO_DMA_MEMORY_SLICED p)
{
    /* If rdmapool is active and this VA is in the pool, free via rdmapool */
    if (IsRdmaPoolAddress(p->drv, p->va)) {
        PLIST_ENTRY entry;
        WdfSpinLockAcquire(p->drv->DmaSpinlock);
        for (entry = p->drv->RdmaPoolAllocList.Flink; entry != &p->drv->RdmaPoolAllocList;
             entry = entry->Flink) {
            PRDMAPOOL_ALLOC_ENTRY candidate =
                CONTAINING_RECORD(entry, RDMAPOOL_ALLOC_ENTRY, ListEntry);
            if (candidate->VirtualAddress == p->va) {
                RemoveEntryList(entry);
                WdfSpinLockRelease(p->drv->DmaSpinlock);
                FreeToRdmaPool(p->drv, p->va, (size_t)candidate->NumPages * PAGE_SIZE);
                ExFreePoolWithTag(candidate, RDMAPOOL_ALLOC_TAG);
                goto done;
            }
        }
        WdfSpinLockRelease(p->drv->DmaSpinlock);
    } else {
        size_t offset;
        FindCommonBuffer(p->drv, p->va, &p->pa, &offset, TRUE);
    }
done:
    ExFreePoolWithTag(p, p->drv->MemoryTag);
}

static PVOID AllocateSlice(PVIRTIO_DMA_MEMORY_SLICED p, PHYSICAL_ADDRESS *ppa)
{
    ULONG offset, index = RtlFindClearBitsAndSet(&p->bitmap, 1, 0);
    if (index >= p->bitmap.SizeOfBitMap) {
        return NULL;
    }
    offset = p->slice * index;
    ppa->QuadPart = p->pa.QuadPart + offset;
    return (PUCHAR)p->va + offset;
}

static void FreeSlice(PVIRTIO_DMA_MEMORY_SLICED p, PVOID va)
{
    size_t offset;

    /* For rdmapool addresses, compute offset directly */
    if (IsRdmaPoolAddress(p->drv, va)) {
        offset = (ULONG_PTR)va - (ULONG_PTR)p->va;
    } else {
        PHYSICAL_ADDRESS pa;
        if (!FindCommonBuffer(p->drv, va, &pa, &offset, FALSE)) {
            DPrintf(0, "%s: block with va %p not found\n", __FUNCTION__, va);
            return;
        }
    }

    if (offset % p->slice) {
        DPrintf(0, "%s: offset %d is wrong for slice %d\n", __FUNCTION__, (ULONG)offset, p->slice);
        return;
    }
    ULONG index = (ULONG)(offset / p->slice);
    if (!RtlTestBit(&p->bitmap, index)) {
        DPrintf(0, "%s: bit %d is NOT set\n", __FUNCTION__, index);
        return;
    }
    RtlClearBit(&p->bitmap, index);
}

PVIRTIO_DMA_MEMORY_SLICED VirtIOWdfDeviceAllocDmaMemorySliced(VirtIODevice *vdev, size_t blockSize,
                                                              ULONG sliceSize)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    size_t allocSize =
        sizeof(VIRTIO_DMA_MEMORY_SLICED) + (blockSize / sliceSize) / 8 + sizeof(ULONG);
    PVIRTIO_DMA_MEMORY_SLICED p =
        ExAllocatePoolUninitialized(NonPagedPool, allocSize, pWdfDriver->MemoryTag);
    if (!p) {
        return NULL;
    }
    __analysis_assume(allocSize > sizeof(*p));
    RtlZeroMemory(p, sizeof(*p));

    /* Allocate the backing DMA buffer */
    if (pWdfDriver->RdmaPoolActive) {
        p->va = AllocateFromRdmaPool(pWdfDriver, blockSize);
        if (p->va) {
            /* Compute PA directly from pool base offsets */
            p->pa.QuadPart = pWdfDriver->RdmaPoolBasePA.QuadPart +
                             ((ULONG_PTR)p->va - (ULONG_PTR)pWdfDriver->RdmaPoolBaseVA);
        }
    } else {
        p->va = AllocateCommonBuffer(pWdfDriver, blockSize, 0);
        p->pa = GetPhysicalAddress(pWdfDriver, p->va);
    }

    if (!p->va || !p->pa.QuadPart) {
        ExFreePoolWithTag(p, pWdfDriver->MemoryTag);
        return NULL;
    }
    p->slice = sliceSize;
    p->drv = pWdfDriver;
    RtlInitializeBitMap(&p->bitmap, p->bitmap_buffer, (ULONG)blockSize / sliceSize);
    p->return_slice = FreeSlice;
    p->get_slice = AllocateSlice;
    p->destroy = FreeSlicedBlock;
    return p;
}

VOID OnDmaTransactionDestroy(WDFOBJECT Object)
{
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(Object);
    DPrintf(1, "%s %p\n", __FUNCTION__, Object);
    // the MDL is one we allocated for the buffer
    // if there is no buffer - this is the MDL provided by the caller
    if (ctx->mdl && ctx->buffer) {
        IoFreeMdl(ctx->mdl);
    }
    if (ctx->buffer) {
        ExFreePoolWithTag(ctx->buffer, ctx->parameters.allocationTag);
    }
}

static FORCEINLINE void RefTransaction(PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx)
{
    InterlockedIncrement(&ctx->refCount);
}

static FORCEINLINE void DerefTransaction(PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx)
{
    if (!InterlockedDecrement(&ctx->refCount)) {
        WdfObjectDelete(ctx->parameters.transaction);
    }
}

BOOLEAN OnDmaTransactionProgramDma(WDFDMATRANSACTION Transaction, WDFDEVICE Device,
                                   WDFCONTEXT Context, WDF_DMA_DIRECTION Direction,
                                   PSCATTER_GATHER_LIST SgList)
{
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(Transaction);
    RefTransaction(ctx);
    ctx->parameters.transaction = Transaction;
    ctx->parameters.sgList = SgList;
    DPrintf(1, "-->%s %p %d frags\n", __FUNCTION__, Transaction, SgList->NumberOfElements);
    BOOLEAN bFailed = !ctx->callback(&ctx->parameters);
    DPrintf(1, "<--%s %s\n", __FUNCTION__, bFailed ? "Failed" : "OK");
    DerefTransaction(ctx);
    return TRUE;
}

static BOOLEAN VirtIOWdfDeviceDmaAsync(VirtIODevice *vdev, PVIRTIO_DMA_TRANSACTION_PARAMS params,
                                       VirtIOWdfDmaTransactionCallback callback,
                                       WDF_DMA_DIRECTION Direction)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    WDFDMATRANSACTION tr;
    WDF_OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VIRTIO_WDF_DMA_TRANSACTION_CONTEXT);
    attr.EvtDestroyCallback = OnDmaTransactionDestroy;
    status = WdfDmaTransactionCreate(pWdfDriver->DmaEnabler, &attr, &tr);
    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s FAILED(create) %X\n", __FUNCTION__, status);
        return FALSE;
    }
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(tr);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->parameters = *params;
    ctx->callback = callback;
    ctx->refCount = 1;
    ctx->direction = Direction;
    if (params->req && params->req != (WDFREQUEST)WDF_INVALID_HANDLE) {
        status = WdfDmaTransactionInitializeUsingRequest(tr, params->req,
                                                         OnDmaTransactionProgramDma, Direction);
    } else if (params->req == (WDFREQUEST)WDF_INVALID_HANDLE) {
        status = WdfDmaTransactionInitializeUsingOffset(tr, OnDmaTransactionProgramDma, Direction,
                                                        (PMDL)params->buffer, 0, params->size);
    } else {
        ctx->buffer = ExAllocatePoolUninitialized(NonPagedPool, ctx->parameters.size,
                                                  ctx->parameters.allocationTag);
        if (ctx->buffer) {
            if (Direction == WdfDmaDirectionWriteToDevice) {
                RtlCopyMemory(ctx->buffer, params->buffer, params->size);
            }
            ctx->mdl = IoAllocateMdl(ctx->buffer, params->size, FALSE, FALSE, NULL);
            if (ctx->mdl) {
                MmBuildMdlForNonPagedPool(ctx->mdl);
                status = WdfDmaTransactionInitialize(tr, OnDmaTransactionProgramDma, Direction,
                                                     ctx->mdl, ctx->buffer, params->size);
            } else {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
        } else {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s FAILED(init) %X\n", __FUNCTION__, status);
        WdfObjectDelete(tr);
        return FALSE;
    }

    status = WdfDmaTransactionExecute(tr, NULL);
    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s FAILED(execution) %X\n", __FUNCTION__, status);
        WdfObjectDelete(tr);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN VirtIOWdfDeviceDmaTxAsync(VirtIODevice *vdev, PVIRTIO_DMA_TRANSACTION_PARAMS params,
                                  VirtIOWdfDmaTransactionCallback callback)
{
    return VirtIOWdfDeviceDmaAsync(vdev, params, callback, WdfDmaDirectionWriteToDevice);
}

BOOLEAN VirtIOWdfDeviceDmaRxAsync(VirtIODevice *vdev, PVIRTIO_DMA_TRANSACTION_PARAMS params,
                                  VirtIOWdfDmaTransactionCallback callback)
{
    return VirtIOWdfDeviceDmaAsync(vdev, params, callback, WdfDmaDirectionReadFromDevice);
}

void VirtIOWdfDeviceDmaTxComplete(VirtIODevice *vdev, WDFDMATRANSACTION transaction)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(transaction);
    NTSTATUS status;
    DPrintf(1, "%s %p\n", __FUNCTION__, transaction);
    WdfDmaTransactionDmaCompletedFinal(transaction, 0, &status);
    DerefTransaction(ctx);
}

void VirtIOWdfDeviceDmaRxComplete(VirtIODevice *vdev, WDFDMATRANSACTION transaction, ULONG length)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(transaction);
    NTSTATUS status;
    DPrintf(1, "%s %p, len %d\n", __FUNCTION__, transaction, length);
    WdfDmaTransactionDmaCompletedFinal(transaction, length, &status);
    if (length && ctx->buffer) {
        RtlCopyMemory(ctx->parameters.buffer, ctx->buffer, length);
    }
    DerefTransaction(ctx);
}

NTSTATUS VirtIOWdfDeviceCheckIOMMUActive(PVIRTIO_WDF_DRIVER pWdfDriver, WDFDEVICE wdfDev)
{
    ULONGLONG deviceFeatures = VirtIOWdfGetDeviceFeatures(pWdfDriver);
    BOOLEAN bHasFeature = virtio_is_feature_enabled(deviceFeatures, VIRTIO_F_ACCESS_PLATFORM);

    DPrintf(0, "%s: VIRTIO_F_ACCESS_PLATFORM is %s\n", __FUNCTION__,
            bHasFeature ? "set" : "not set");

    // https://learn.microsoft.com/en-us/windows-hardware/drivers/pci/enabling-dma-remapping-for-device-drivers

    const DEVPROPKEY propKey = {
        { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 18
    };
    ULONG value = 0, reqSize = 0;
    DEVPROPTYPE propType;
    WDF_DEVICE_PROPERTY_DATA propData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&propData, &propKey);
    NTSTATUS status =
        WdfDeviceQueryPropertyEx(wdfDev, &propData, sizeof(value), &value, &reqSize, &propType);
    DPrintf(0, "%s: status %X, dma remap=%d\n", __FUNCTION__, status, value);
    pWdfDriver->IsIoMmuActive = bHasFeature && value == 2;
    if (!NT_SUCCESS(status) || value != 2 || bHasFeature) {
        return STATUS_SUCCESS;
    }
    pWdfDriver->IsIoMmuActive = FALSE;

    // the VIRTIO_F_ACCESS_PLATFORM is not set and there is
    // a possibility of DMA remapping
    WDFCOMMONBUFFER commonBuffer = NULL;
    status = WdfCommonBufferCreate(pWdfDriver->DmaEnabler, PAGE_SIZE, WDF_NO_OBJECT_ATTRIBUTES,
                                   &commonBuffer);
    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s: Can't allocate common buffer\n", __FUNCTION__);
        return status;
    }

    // let's check whether the physical address returned from common buffer API
    // is the same as returned from plain MmGetPhysicalAddress
    PHYSICAL_ADDRESS pa = WdfCommonBufferGetAlignedLogicalAddress(commonBuffer);
    PVOID va = WdfCommonBufferGetAlignedVirtualAddress(commonBuffer);
    PHYSICAL_ADDRESS plain = MmGetPhysicalAddress(va);
    DPrintf(0, "%s: buffer at %I64X, plain %I64X\n", __FUNCTION__, pa.QuadPart, plain.QuadPart);
    status = plain.QuadPart == pa.QuadPart ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
    WdfObjectDelete(commonBuffer);
    return status;
}
