/*
 * Restricted DMA Pool Driver - Public Interface Header
 *
 * This header defines the device interface GUID, IOCTL codes, and
 * data structures shared between the rdmapool driver and its clients
 * (e.g., VirtIO WDF library).
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

/*
 * Include <initguid.h> before this header in exactly ONE .c file per module
 * to define the GUID. All other .c files will get an extern reference.
 */

/*
 * Device interface GUID for the restricted DMA pool driver.
 * Clients use IoGetDeviceInterfaces with this GUID to discover
 * the rdmapool device.
 *
 * {7B5E2F3A-9C1D-4E8F-A6B2-3D4C5E6F7A8B}
 */
DEFINE_GUID(GUID_DEVINTERFACE_RDMAPOOL, 0x7b5e2f3a, 0x9c1d, 0x4e8f, 0xa6, 0xb2, 0x3d, 0x4c, 0x5e, 0x6f, 0x7a, 0x8b);

/*
 * IOCTL definitions for rdmapool driver.
 * Uses FILE_DEVICE_UNKNOWN with function codes starting at 0x800.
 */
#define FILE_DEVICE_RDMAPOOL      0x8000

/*
 * IOCTL_RDMAPOOL_ALLOCATE
 *   Allocate contiguous pages from the restricted DMA pool.
 *   Input:  RDMAPOOL_ALLOCATE_INPUT
 *   Output: RDMAPOOL_ALLOCATE_OUTPUT
 */
#define IOCTL_RDMAPOOL_ALLOCATE   CTL_CODE(FILE_DEVICE_RDMAPOOL, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_RDMAPOOL_FREE
 *   Free previously allocated pages back to the restricted DMA pool.
 *   Input:  RDMAPOOL_FREE_INPUT
 *   Output: None
 */
#define IOCTL_RDMAPOOL_FREE       CTL_CODE(FILE_DEVICE_RDMAPOOL, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_RDMAPOOL_QUERY_POOL
 *   Query pool information (base addresses and total size).
 *   Input:  None
 *   Output: RDMAPOOL_QUERY_POOL_OUTPUT
 */
#define IOCTL_RDMAPOOL_QUERY_POOL CTL_CODE(FILE_DEVICE_RDMAPOOL, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_RDMAPOOL_RESERVE
 *   Mark pages as reserved in the bitmap without allocating or zeroing.
 *   Used by drivers (e.g., viostor) that obtain pool base via QUERY_POOL
 *   and manage their own sub-allocation, to prevent overlap with other
 *   clients that use ALLOCATE.
 *   Input:  RDMAPOOL_RESERVE_INPUT
 *   Output: None
 */
#define IOCTL_RDMAPOOL_RESERVE    CTL_CODE(FILE_DEVICE_RDMAPOOL, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

typedef struct _RDMAPOOL_ALLOCATE_INPUT
{
    ULONG NumPages; /* Number of PAGE_SIZE pages to allocate */
} RDMAPOOL_ALLOCATE_INPUT, *PRDMAPOOL_ALLOCATE_INPUT;

typedef struct _RDMAPOOL_ALLOCATE_OUTPUT
{
    PVOID VirtualAddress;             /* Kernel VA of allocated region */
    PHYSICAL_ADDRESS PhysicalAddress; /* Physical address of allocated region */
} RDMAPOOL_ALLOCATE_OUTPUT, *PRDMAPOOL_ALLOCATE_OUTPUT;

typedef struct _RDMAPOOL_FREE_INPUT
{
    PVOID VirtualAddress; /* VA returned by ALLOCATE */
    ULONG NumPages;       /* Number of pages to free */
} RDMAPOOL_FREE_INPUT, *PRDMAPOOL_FREE_INPUT;

typedef struct _RDMAPOOL_QUERY_POOL_OUTPUT
{
    PVOID BaseVirtualAddress;             /* Kernel VA of pool base */
    PHYSICAL_ADDRESS BasePhysicalAddress; /* Physical address of pool base */
    ULONG64 TotalSize;                    /* Total pool size in bytes */
} RDMAPOOL_QUERY_POOL_OUTPUT, *PRDMAPOOL_QUERY_POOL_OUTPUT;

typedef struct _RDMAPOOL_RESERVE_INPUT
{
    ULONG NumPages; /* Number of pages from pool start to mark reserved */
} RDMAPOOL_RESERVE_INPUT, *PRDMAPOOL_RESERVE_INPUT;

#pragma pack(pop)
