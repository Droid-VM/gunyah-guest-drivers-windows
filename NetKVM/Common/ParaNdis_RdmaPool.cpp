/*
 * NetKVM Restricted DMA Pool support - implementation.
 * See ParaNdis_RdmaPool.h. Mirrors VirtIO/WDF/VirtIOWdf.c + Dma.c.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#include "ndis56common.h"
#include "kdebugprint.h"
#include "ParaNdis_RdmaPool.h"

#include <wdmguid.h>
#include <initguid.h>
#include "rdmapool_interface.h"

#include "Trace.h"
#ifdef NETKVM_WPP_ENABLED
#include "ParaNdis_RdmaPool.tmh"
#endif

/* Issue a synchronous IOCTL to the rdmapool device. PASSIVE_LEVEL only. */
static NTSTATUS RdmaPoolIoctl(PARANDIS_ADAPTER *pContext,
                              ULONG ControlCode,
                              PVOID InBuf,
                              ULONG InLen,
                              PVOID OutBuf,
                              ULONG OutLen)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION irpStack;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(ControlCode,
                                        pContext->RdmaPoolDeviceObject,
                                        InBuf,
                                        InLen,
                                        OutBuf,
                                        OutLen,
                                        FALSE,
                                        &event,
                                        &iosb);
    if (irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->FileObject = pContext->RdmaPoolFileObject;

    status = IoCallDriver(pContext->RdmaPoolDeviceObject, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

NTSTATUS ParaNdis_RdmaPoolConnect(PARANDIS_ADAPTER *pContext)
{
    NTSTATUS status;
    PWSTR deviceInterfaceList = NULL;
    UNICODE_STRING deviceName;
    RDMAPOOL_QUERY_POOL_OUTPUT queryOutput;

    pContext->RdmaPoolActive = FALSE;
    pContext->RdmaPoolDeviceObject = NULL;
    pContext->RdmaPoolFileObject = NULL;
    pContext->RdmaPoolBaseVA = NULL;
    pContext->RdmaPoolBasePA.QuadPart = 0;
    pContext->RdmaPoolSize = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_RDMAPOOL, NULL, 0, &deviceInterfaceList);
    if (!NT_SUCCESS(status) || deviceInterfaceList == NULL || *deviceInterfaceList == L'\0')
    {
        DPrintf(0, "rdmapool device interface not found (0x%x) - using normal DMA", status);
        if (deviceInterfaceList)
        {
            ExFreePool(deviceInterfaceList);
        }
        return STATUS_NOT_FOUND;
    }

    RtlInitUnicodeString(&deviceName, deviceInterfaceList);
    status = IoGetDeviceObjectPointer(&deviceName,
                                      FILE_ALL_ACCESS,
                                      &pContext->RdmaPoolFileObject,
                                      &pContext->RdmaPoolDeviceObject);
    ExFreePool(deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DPrintf(0, "IoGetDeviceObjectPointer(rdmapool) failed 0x%x", status);
        pContext->RdmaPoolFileObject = NULL;
        pContext->RdmaPoolDeviceObject = NULL;
        return status;
    }

    RtlZeroMemory(&queryOutput, sizeof(queryOutput));
    status = RdmaPoolIoctl(pContext, (ULONG)IOCTL_RDMAPOOL_QUERY_POOL, NULL, 0, &queryOutput, sizeof(queryOutput));
    if (!NT_SUCCESS(status))
    {
        DPrintf(0, "IOCTL_RDMAPOOL_QUERY_POOL failed 0x%x", status);
        ObDereferenceObject(pContext->RdmaPoolFileObject);
        pContext->RdmaPoolFileObject = NULL;
        pContext->RdmaPoolDeviceObject = NULL;
        return status;
    }

    pContext->RdmaPoolBaseVA = queryOutput.BaseVirtualAddress;
    pContext->RdmaPoolBasePA = queryOutput.BasePhysicalAddress;
    pContext->RdmaPoolSize = queryOutput.TotalSize;
    pContext->RdmaPoolActive = TRUE;

    DPrintf(0,
            "Connected to rdmapool VA=%p PA=0x%llx Size=0x%llx",
            pContext->RdmaPoolBaseVA,
            pContext->RdmaPoolBasePA.QuadPart,
            pContext->RdmaPoolSize);
    return STATUS_SUCCESS;
}

VOID ParaNdis_RdmaPoolDisconnect(PARANDIS_ADAPTER *pContext)
{
    if (pContext->RdmaPoolFileObject != NULL)
    {
        ObDereferenceObject(pContext->RdmaPoolFileObject);
        pContext->RdmaPoolFileObject = NULL;
    }
    pContext->RdmaPoolDeviceObject = NULL;
    pContext->RdmaPoolActive = FALSE;
    pContext->RdmaPoolBaseVA = NULL;
    pContext->RdmaPoolSize = 0;
}

PVOID ParaNdis_RdmaPoolAllocate(PARANDIS_ADAPTER *pContext, ULONG size, PHYSICAL_ADDRESS *pPa)
{
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;
    NTSTATUS status;

    if (pPa)
    {
        pPa->QuadPart = 0;
    }
    if (!pContext->RdmaPoolActive || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NULL;
    }

    allocInput.NumPages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));

    status = RdmaPoolIoctl(pContext,
                           (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                           &allocInput,
                           sizeof(allocInput),
                           &allocOutput,
                           sizeof(allocOutput));
    if (!NT_SUCCESS(status))
    {
        DPrintf(0, "IOCTL_RDMAPOOL_ALLOCATE failed 0x%x (size=0x%x)", status, size);
        return NULL;
    }

    if (pPa)
    {
        *pPa = allocOutput.PhysicalAddress;
    }
    DPrintf(2,
            "rdmapool alloc VA=%p PA=0x%llx size=0x%x",
            allocOutput.VirtualAddress,
            allocOutput.PhysicalAddress.QuadPart,
            size);
    return allocOutput.VirtualAddress;
}

VOID ParaNdis_RdmaPoolFree(PARANDIS_ADAPTER *pContext, PVOID va, ULONG size)
{
    RDMAPOOL_FREE_INPUT freeInput;

    if (!pContext->RdmaPoolActive || va == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return;
    }
    freeInput.VirtualAddress = va;
    freeInput.NumPages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    (void)RdmaPoolIoctl(pContext, (ULONG)IOCTL_RDMAPOOL_FREE, &freeInput, sizeof(freeInput), NULL, 0);
}

BOOLEAN ParaNdis_RdmaPoolContains(PARANDIS_ADAPTER *pContext, PVOID va)
{
    ULONG_PTR base;
    ULONG_PTR addr;

    if (!pContext->RdmaPoolActive || pContext->RdmaPoolBaseVA == NULL || va == NULL)
    {
        return FALSE;
    }
    base = (ULONG_PTR)pContext->RdmaPoolBaseVA;
    addr = (ULONG_PTR)va;
    return (addr >= base && addr < base + pContext->RdmaPoolSize);
}
