/*
 * NetKVM Restricted DMA Pool support
 *
 * Lets NetKVM allocate device-visible (DMA) memory from the rdmapool
 * restricted DMA pool driver when running inside a Gunyah protected VM,
 * where normal guest memory is not accessible to the virtio backend.
 *
 * Mirrors the approach already used by VirtIO/WDF (VirtIOWdf.c / Dma.c)
 * and viostor (viostor_bounce.c).
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct _PARANDIS_ADAPTER *PPARANDIS_ADAPTER_FWD;

    /*
     * Try to connect to the rdmapool device. On success sets
     * pContext->RdmaPoolActive = TRUE and records the pool base VA/PA/size.
     * Safe to call when rdmapool is absent (returns failure, RdmaPoolActive
     * stays FALSE and NetKVM keeps using normal NdisMAllocateSharedMemory).
     * Must be called at PASSIVE_LEVEL.
     */
    NTSTATUS ParaNdis_RdmaPoolConnect(PPARANDIS_ADAPTER_FWD pContext);

    /* Release the rdmapool device reference. */
    VOID ParaNdis_RdmaPoolDisconnect(PPARANDIS_ADAPTER_FWD pContext);

    /*
     * Allocate 'size' bytes (rounded up to pages) from the restricted DMA
     * pool. Returns the kernel VA and fills *pPa with the physical address.
     * Returns NULL on failure. Must be called at PASSIVE_LEVEL.
     */
    PVOID ParaNdis_RdmaPoolAllocate(PPARANDIS_ADAPTER_FWD pContext, ULONG size, PHYSICAL_ADDRESS *pPa);

    /* Free a previous ParaNdis_RdmaPoolAllocate. Must be at PASSIVE_LEVEL. */
    VOID ParaNdis_RdmaPoolFree(PPARANDIS_ADAPTER_FWD pContext, PVOID va, ULONG size);

    /* TRUE if 'va' lies within the connected restricted DMA pool region. */
    BOOLEAN ParaNdis_RdmaPoolContains(PPARANDIS_ADAPTER_FWD pContext, PVOID va);

#ifdef __cplusplus
}
#endif
