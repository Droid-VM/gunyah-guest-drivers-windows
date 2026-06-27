/*
 * Restricted DMA Pool - Bitmap Page Allocator Header
 *
 * Page-granularity bitmap allocator for the restricted DMA pool region.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

#include <ntddk.h>

/*
 * Initialize the DMA pool allocator.
 *
 * @param PhysicalBase  Physical base address of the pool region.
 * @param VirtualBase   Kernel virtual address of the mapped pool region.
 * @param TotalSize     Total pool size in bytes (must be page-aligned).
 * @return STATUS_SUCCESS or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
DmaPoolInit(_In_ PHYSICAL_ADDRESS PhysicalBase, _In_ PVOID VirtualBase, _In_ SIZE_T TotalSize);

/*
 * Destroy the DMA pool allocator and free internal resources.
 */
VOID DmaPoolDestroy(VOID);

/*
 * Allocate contiguous pages from the restricted DMA pool.
 *
 * @param NumPages          Number of pages to allocate.
 * @param VirtualAddress    Receives the kernel VA of the allocation.
 * @param PhysicalAddress   Receives the physical address of the allocation.
 * @return STATUS_SUCCESS or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
DmaPoolAllocatePages(_In_ ULONG NumPages, _Out_ PVOID *VirtualAddress, _Out_ PHYSICAL_ADDRESS *PhysicalAddress);

/*
 * Free pages previously allocated from the restricted DMA pool.
 *
 * @param VirtualAddress    VA returned by DmaPoolAllocatePages.
 * @param NumPages          Number of pages to free.
 */
VOID DmaPoolFreePages(_In_ PVOID VirtualAddress, _In_ ULONG NumPages);

/*
 * Query pool base addresses and total size.
 */
VOID DmaPoolQueryInfo(_Out_ PVOID *BaseVirtualAddress,
                      _Out_ PHYSICAL_ADDRESS *BasePhysicalAddress,
                      _Out_ ULONG64 *TotalSize);

/*
 * Reserve pages at the start of the pool (mark as allocated in bitmap).
 * Does not zero memory or return addresses.
 * Used by clients that obtain the pool base via QueryInfo and manage
 * their own sub-allocation.
 *
 * @param NumPages  Number of pages from pool start to mark as reserved.
 * @return STATUS_SUCCESS or STATUS_INVALID_PARAMETER.
 */
NTSTATUS
DmaPoolReservePages(_In_ ULONG NumPages);
