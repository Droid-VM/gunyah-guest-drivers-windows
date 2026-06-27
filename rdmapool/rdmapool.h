/*
 * Restricted DMA Pool Driver - Internal Header
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>

#define RDMAPOOL_TAG 'PMDR'

/*
 * Device context for the rdmapool WDFDEVICE.
 */
typedef struct _RDMAPOOL_DEVICE_CONTEXT
{
    PHYSICAL_ADDRESS PoolPhysicalBase;
    SIZE_T PoolSize;
    PVOID PoolVirtualBase;
    BOOLEAN PoolInitialized;
} RDMAPOOL_DEVICE_CONTEXT, *PRDMAPOOL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RDMAPOOL_DEVICE_CONTEXT, RdmaPoolGetDeviceContext)

/* Driver entry points */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD RdmaPoolEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE RdmaPoolEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE RdmaPoolEvtDeviceReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RdmaPoolEvtIoDeviceControl;
