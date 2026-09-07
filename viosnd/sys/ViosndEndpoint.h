// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "ViosndFormat.h"

/*
 * Grouping the device's PCM streams into the endpoints Windows will show.
 *
 * virtio-snd hands over a flat list of PCM streams, and the only key the spec makes unique is
 * `stream_id` -- the stream's position in that list. `hda_fn_nid` is an informational grouping
 * label with no uniqueness behind it at all: QEMU reports 0 on every stream it offers. Keying an
 * endpoint on the nid therefore silently discards every stream after the first in a direction on
 * such a device, so identity here is the stream_id, and each stream the device offers becomes an
 * endpoint of its own.
 *
 * PortCls exposes one streaming pin per endpoint, so an endpoint carries exactly one stream and a
 * device with several of them needs one endpoint each to reach them all. How many a card can
 * carry is bounded by the subdevice names below, and by nothing else.
 *
 * The nid is still carried, because two things do read it. It indexes the host's vendor hints,
 * which are published per direction and per host device. And it is what says a render stream and
 * a capture stream are the two ends of one host device -- an association taken here in arrival
 * order, the k-th unpaired render stream on a nid against the k-th unpaired capture stream on it.
 * Whatever a direction has left over is an endpoint with no counterpart, which is an ordinary
 * card and not a shortfall.
 */

/* Windows binds subdevice names from static strings in the INF, so the count is fixed at build
 * time. Anything past this is dropped -- loudly, because a silently missing endpoint looks
 * exactly like a host that never offered it. */
#define VIOSND_MAX_ENDPOINTS 4u

/* No stream. Stream 0 is a real stream, so zero cannot carry this and the field has to be set
 * explicitly -- zeroing the endpoint would otherwise read as "paired with stream 0". */
#define VIOSND_NO_PEER       MAXULONG

typedef struct _VIOSND_ENDPOINT
{
    ULONG StreamId;    /* virtio PCM stream backing this endpoint; the endpoint's identity */
    ULONG DeviceIndex; /* hda_fn_nid of the host device it belongs to. Not unique -- see above. */
    /* The stream backing this endpoint's counterpart in the other direction on the same host
     * device, or VIOSND_NO_PEER when that nid had none left to give. */
    ULONG PeerStreamId;
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
