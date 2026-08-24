// SPDX-License-Identifier: BSD-3-Clause
#include "precomp.h"

/*
 * The KS node type an endpoint kind presents as.
 *
 * Windows builds the endpoint's displayed name from this, and picks the icon beside it from the
 * same place -- so a headset that presents as KSNODETYPE_SPEAKER is not merely mislabelled, it
 * is indistinguishable from the phone's own speaker in the list the user picks from. Falling
 * back on direction alone is what the driver did before, and it is why every endpoint read
 * "Speakers" no matter what was on the other end.
 */
static const GUID *ViosndNodeTypeForKind(_In_ ULONG Kind, _In_ BOOLEAN Capture)
{
    switch (Kind)
    {
        case VIOSND_ENDPOINT_KIND_SPEAKER:
            return &KSNODETYPE_SPEAKER;
        case VIOSND_ENDPOINT_KIND_HEADPHONES:
            return &KSNODETYPE_HEADPHONES;
        case VIOSND_ENDPOINT_KIND_HEADSET:
            return Capture ? &KSNODETYPE_MICROPHONE : &KSNODETYPE_HEADSET;
        case VIOSND_ENDPOINT_KIND_LINE_OUT:
            return &KSNODETYPE_LINE_CONNECTOR;
        case VIOSND_ENDPOINT_KIND_DIGITAL:
            return &KSNODETYPE_SPDIF_INTERFACE;
        case VIOSND_ENDPOINT_KIND_MICROPHONE:
            return &KSNODETYPE_MICROPHONE;
        case VIOSND_ENDPOINT_KIND_TELEPHONY:
            return &KSNODETYPE_TELEPHONE;
        default:
            /* The host said nothing. Direction is all that is left, and it is what the driver used
             * to assume for everything. */
            return Capture ? &KSNODETYPE_MICROPHONE : &KSNODETYPE_SPEAKER;
    }
}

static VOID ViosndHostHint(_In_opt_ const VIOSND_VENDOR_CONFIG *VendorConfig,
                           _In_ BOOLEAN Capture,
                           _In_ ULONG DeviceIndex,
                           _Out_ PULONG Rate,
                           _Out_ PUCHAR Channels,
                           _Out_ PULONG Kind)
{
    *Rate = 0;
    *Channels = 0;
    *Kind = VIOSND_ENDPOINT_KIND_UNKNOWN;

    if (VendorConfig == NULL || VendorConfig->version < 2u)
    {
        return;
    }

    const VIOSND_VENDOR_PREFERRED *table;
    ULONG count;
    if (Capture)
    {
        table = VendorConfig->preferred_input;
        count = VendorConfig->preferred_input_count;
    }
    else
    {
        table = VendorConfig->preferred_output;
        count = VendorConfig->preferred_output_count;
    }
    if (DeviceIndex >= count || DeviceIndex >= VIOSND_VENDOR_CFG_MAX_DEVICES)
    {
        return;
    }
    *Rate = table[DeviceIndex].rate;
    *Channels = (UCHAR)min(table[DeviceIndex].channels, 255u);
    *Kind = table[DeviceIndex].kind;
}

static PVIOSND_ENDPOINT ViosndFindEndpoint(_In_ PVIOSND_ENDPOINT_SET Set, _In_ BOOLEAN Capture, _In_ ULONG DeviceIndex)
{
    PVIOSND_ENDPOINT list = Capture ? Set->Capture : Set->Render;
    ULONG count = Capture ? Set->CaptureCount : Set->RenderCount;

    for (ULONG i = 0; i < count; ++i)
    {
        if (list[i].DeviceIndex == DeviceIndex)
        {
            return &list[i];
        }
    }
    return NULL;
}

NTSTATUS
ViosndGroupEndpoints(_In_reads_(InfoCount) const VIRTIO_SND_PCM_INFO *Info,
                     _In_ ULONG InfoCount,
                     _In_opt_ const VIOSND_VENDOR_CONFIG *VendorConfig,
                     _Out_ PVIOSND_ENDPOINT_SET Set)
{
    RtlZeroMemory(Set, sizeof(*Set));

    for (ULONG i = 0; i < InfoCount; ++i)
    {
        const VIRTIO_SND_PCM_INFO *info = &Info[i];
        BOOLEAN capture;

        if (info->direction == VIRTIO_SND_D_OUTPUT)
        {
            capture = FALSE;
        }
        else if (info->direction == VIRTIO_SND_D_INPUT)
        {
            capture = TRUE;
        }
        else
        {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u has direction %u, ignoring\n",
                       i,
                       info->direction);
            Set->DroppedStreams++;
            continue;
        }

        ULONG deviceIndex = info->hdr.hda_fn_nid;

        /* The first stream on a nid becomes the endpoint; later ones are surplus. */
        if (ViosndFindEndpoint(Set, capture, deviceIndex) != NULL)
        {
            Set->DroppedStreams++;
            continue;
        }

        PULONG count = capture ? &Set->CaptureCount : &Set->RenderCount;
        if (*count >= VIOSND_MAX_ENDPOINTS)
        {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: %s device %u dropped: this driver exposes at most %u per "
                       "direction\n",
                       capture ? "capture" : "render",
                       deviceIndex,
                       VIOSND_MAX_ENDPOINTS);
            Set->DroppedStreams++;
            continue;
        }

        VIOSND_FORMAT_CAPS caps;
        caps.Formats = info->formats;
        caps.Rates = info->rates;
        caps.ChannelsMin = info->channels_min;
        caps.ChannelsMax = info->channels_max;

        ULONG hintRate;
        UCHAR hintChannels;
        ULONG hintKind;
        ViosndHostHint(VendorConfig, capture, deviceIndex, &hintRate, &hintChannels, &hintKind);

        VIOSND_WAVE_FORMAT preferred;
        if (!ViosndPickPreferred(&caps, hintRate, hintChannels, &preferred))
        {
            /* The stream offers nothing Windows can express -- companded or DSD only, or a
             * channel range that excludes everything. Not an error in the driver; the device
             * simply has nothing to give this OS. */
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: %s device %u offers no Windows-expressible format "
                       "(formats=0x%llx rates=0x%llx ch=%u..%u)\n",
                       capture ? "capture" : "render",
                       deviceIndex,
                       caps.Formats,
                       caps.Rates,
                       caps.ChannelsMin,
                       caps.ChannelsMax);
            Set->DroppedStreams++;
            continue;
        }

        PVIOSND_ENDPOINT endpoint = capture ? &Set->Capture[*count] : &Set->Render[*count];
        RtlZeroMemory(endpoint, sizeof(*endpoint));
        endpoint->StreamId = i;
        endpoint->DeviceIndex = deviceIndex;
        endpoint->Capture = capture;
        endpoint->Caps = caps;
        endpoint->Preferred = preferred;
        endpoint->PreferredFromHost = (hintRate != 0 && preferred.SampleRate == hintRate) ? TRUE : FALSE;
        endpoint->Kind = hintKind;
        endpoint->NodeType = ViosndNodeTypeForKind(hintKind, capture);
        (*count)++;

        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: %s endpoint %u <- stream %u, kind=%u, default %uHz %uch %ubit%s\n",
                   capture ? "capture" : "render",
                   deviceIndex,
                   i,
                   hintKind,
                   preferred.SampleRate,
                   preferred.Channels,
                   preferred.ContainerBits,
                   endpoint->PreferredFromHost ? " (host's own rate)" : "");
    }

    if (Set->RenderCount == 0 && Set->CaptureCount == 0)
    {
        return STATUS_NOT_FOUND;
    }
    return STATUS_SUCCESS;
}
