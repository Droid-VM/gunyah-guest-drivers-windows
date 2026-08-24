// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "ViosndFormat.h"

/*
 * Grouping the device's PCM streams into the endpoints Windows will show.
 *
 * virtio-snd hands over a flat list of PCM streams. What ties several of them into one logical
 * device is `hda_fn_nid`, which the spec also uses to associate a stream with its jack and its
 * channel map -- so it, and not the stream's position in the list, is the device identity. The
 * two directions number their nids independently: output device 0 and input device 0 both
 * report nid 0.
 *
 * PortCls exposes one streaming pin per endpoint, so one stream per endpoint is taken. Where a
 * device offers several streams on the same nid the rest are surplus: real hardware uses them
 * for simultaneous playback into one converter, which is not something this driver does.
 */

/* Windows binds subdevice names from static strings in the INF, so the count is fixed at build
 * time. Anything past this is dropped -- loudly, because a silently missing endpoint looks
 * exactly like a host that never offered it. */
#define VIOSND_MAX_ENDPOINTS 4u

typedef struct _VIOSND_ENDPOINT
{
    ULONG StreamId;    /* virtio PCM stream backing this endpoint */
    ULONG DeviceIndex; /* hda_fn_nid: which host device it is pinned to */
    BOOLEAN Capture;
    VIOSND_FORMAT_CAPS Caps;
    /* What to offer as the default format. Always valid: the host's hint when it gave one,
     * otherwise the best the stream itself can do. */
    VIOSND_WAVE_FORMAT Preferred;
    /* Whether the host actually named it, as opposed to this being the driver's own pick.
     * Only diagnostics depend on the difference. */
    BOOLEAN PreferredFromHost;
    /* VIOSND_ENDPOINT_KIND_*, as the host described it. */
    ULONG Kind;
    /* The KS node type that kind maps to. This is what decides the name Windows shows for the
     * endpoint and the icon beside it, so it is the whole reason the kind is carried at all. */
    const GUID *NodeType;
} VIOSND_ENDPOINT, *PVIOSND_ENDPOINT;

typedef struct _VIOSND_ENDPOINT_SET
{
    VIOSND_ENDPOINT Render[VIOSND_MAX_ENDPOINTS];
    ULONG RenderCount;
    VIOSND_ENDPOINT Capture[VIOSND_MAX_ENDPOINTS];
    ULONG CaptureCount;
    /* Streams the device offered that did not become endpoints, for the log. */
    ULONG DroppedStreams;
} VIOSND_ENDPOINT_SET, *PVIOSND_ENDPOINT_SET;

/*
 * Groups `Info` into endpoints. `VendorConfig` may be NULL, or carry no hints; either way every
 * endpoint comes back with a usable Preferred.
 *
 * Returns STATUS_NOT_FOUND when nothing in the list can be expressed as a Windows format --
 * which is a real answer about the device, not a failure to parse it.
 */
NTSTATUS ViosndGroupEndpoints(_In_reads_(InfoCount) const VIRTIO_SND_PCM_INFO *Info,
                              _In_ ULONG InfoCount,
                              _In_opt_ const VIOSND_VENDOR_CONFIG *VendorConfig,
                              _Out_ PVIOSND_ENDPOINT_SET Set);
