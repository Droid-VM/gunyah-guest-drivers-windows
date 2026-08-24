// SPDX-License-Identifier: BSD-3-Clause
#include "precomp.h"

/* Indexed by VIRTIO_SND_PCM_RATE_*. 0 marks an enumerator this driver does not handle. */
static const ULONG ViosndRateTable[] = {5512,
                                        8000,
                                        11025,
                                        16000,
                                        22050,
                                        32000,
                                        44100,
                                        48000,
                                        64000,
                                        88200,
                                        96000,
                                        176400,
                                        192000,
                                        384000};

ULONG
ViosndRateHz(_In_ UCHAR VirtioRate)
{
    if (VirtioRate >= SIZEOF_ARRAY(ViosndRateTable))
    {
        return 0;
    }
    return ViosndRateTable[VirtioRate];
}

BOOLEAN
ViosndRateFromHz(_In_ ULONG Hz, _Out_ PUCHAR VirtioRate)
{
    *VirtioRate = 0;
    for (UCHAR i = 0; i < (UCHAR)SIZEOF_ARRAY(ViosndRateTable); ++i)
    {
        if (ViosndRateTable[i] == Hz)
        {
            *VirtioRate = i;
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN
ViosndFormatBits(_In_ UCHAR VirtioFormat, _Out_ PUCHAR ContainerBits, _Out_ PUCHAR ValidBits, _Out_ PBOOLEAN Float)
{
    *ContainerBits = 0;
    *ValidBits = 0;
    *Float = FALSE;

    switch (VirtioFormat)
    {
        case VIRTIO_SND_PCM_FMT_U8:
            /* Windows' 8-bit PCM is unsigned and everything wider is signed, which is the same
             * convention virtio uses -- so U8 and S16/S24/S32 map across without a sign flip, and
             * S8/U16 have no Windows representation at all. */
            *ContainerBits = 8;
            *ValidBits = 8;
            return TRUE;
        case VIRTIO_SND_PCM_FMT_S16:
            *ContainerBits = 16;
            *ValidBits = 16;
            return TRUE;
        case VIRTIO_SND_PCM_FMT_S24_3:
            *ContainerBits = 24;
            *ValidBits = 24;
            return TRUE;
        case VIRTIO_SND_PCM_FMT_S24:
            *ContainerBits = 32;
            *ValidBits = 24;
            return TRUE;
        case VIRTIO_SND_PCM_FMT_S32:
            *ContainerBits = 32;
            *ValidBits = 32;
            return TRUE;
        case VIRTIO_SND_PCM_FMT_FLOAT:
            *ContainerBits = 32;
            *ValidBits = 32;
            *Float = TRUE;
            return TRUE;
        default:
            return FALSE;
    }
}

ULONG
ViosndFrameBytes(_In_ const VIOSND_WAVE_FORMAT *Format)
{
    return (ULONG)Format->Channels * ((ULONG)Format->ContainerBits / 8u);
}

/* Formats worth offering, best first: the order decides which one wins when the host's hint
 * names only a rate. Wider containers first so a host that can carry more is not pinned to 16
 * bits by an accident of enumeration order. */
/*
 * Float first, then integers widest to narrowest.
 *
 * The order decides what the endpoint defaults to, and float is what both ends of the chain
 * already use: the Windows audio engine mixes in float, and the Android endpoints this device
 * plays to run in float. Choosing anything else means the samples are converted twice on the way
 * out and twice on the way back, to arrive in the format they started in.
 *
 * It only ever decides ties. A request is matched on its container width, its significant bits
 * and whether it is float, so an integer request still lands on an integer format however this
 * list is ordered.
 */
static const UCHAR ViosndFormatPreference[] = {VIRTIO_SND_PCM_FMT_FLOAT,
                                               VIRTIO_SND_PCM_FMT_S32,
                                               VIRTIO_SND_PCM_FMT_S24,
                                               VIRTIO_SND_PCM_FMT_S24_3,
                                               VIRTIO_SND_PCM_FMT_S16,
                                               VIRTIO_SND_PCM_FMT_U8};

static VOID ViosndFillRange(_Out_ KSDATARANGE_AUDIO *Range,
                            _In_ const VIOSND_FORMAT_CAPS *Caps,
                            _In_ UCHAR ContainerBits,
                            _In_ BOOLEAN Float,
                            _In_ ULONG Hz)
{
    RtlZeroMemory(Range, sizeof(*Range));
    Range->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    Range->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    Range->DataRange.SubFormat = Float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    Range->DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    Range->MaximumChannels = Caps->ChannelsMax;
    Range->MinimumBitsPerSample = ContainerBits;
    Range->MaximumBitsPerSample = ContainerBits;
    /* One entry per rate rather than a min..max span: virtio's rates are a set of discrete
     * enumerators with gaps in it, and a span would promise everything in between. */
    Range->MinimumSampleFrequency = Hz;
    Range->MaximumSampleFrequency = Hz;
}

NTSTATUS
ViosndBuildDataRanges(_In_ const VIOSND_FORMAT_CAPS *Caps,
                      _In_opt_ const VIOSND_WAVE_FORMAT *Preferred,
                      _Outptr_result_buffer_(*RangeCount) PKSDATARANGE **RangePointers,
                      _Out_ PULONG RangeCount)
{
    ULONG count = 0;

    *RangePointers = NULL;
    *RangeCount = 0;

    if (Caps->ChannelsMax == 0 || Caps->ChannelsMin > Caps->ChannelsMax)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (ULONG f = 0; f < SIZEOF_ARRAY(ViosndFormatPreference); ++f)
    {
        UCHAR container, valid;
        BOOLEAN isFloat;
        if ((Caps->Formats & (1ULL << ViosndFormatPreference[f])) == 0)
        {
            continue;
        }
        if (!ViosndFormatBits(ViosndFormatPreference[f], &container, &valid, &isFloat))
        {
            continue;
        }
        for (UCHAR r = 0; r < (UCHAR)SIZEOF_ARRAY(ViosndRateTable); ++r)
        {
            if ((Caps->Rates & (1ULL << r)) != 0 && ViosndRateTable[r] != 0)
            {
                count++;
            }
        }
    }

    if (count == 0)
    {
        /* Nothing in common. The caller reports the stream as unusable rather than inventing a
         * format the host never offered. */
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Preferred != NULL)
    {
        count++; /* it goes in front, in addition to its place in the enumeration */
    }

    /* One allocation: the pointer array Windows walks, then the ranges it points at. */
    SIZE_T pointerBytes = count * sizeof(PKSDATARANGE);
    SIZE_T rangeBytes = count * sizeof(KSDATARANGE_AUDIO);
    PUCHAR block = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx, pointerBytes + rangeBytes, VIOSND_POOL_TAG);
    if (block == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PKSDATARANGE *pointers = (PKSDATARANGE *)block;
    KSDATARANGE_AUDIO *ranges = (KSDATARANGE_AUDIO *)(block + pointerBytes);

    ULONG i = 0;
    if (Preferred != NULL)
    {
        ViosndFillRange(&ranges[i], Caps, Preferred->ContainerBits, Preferred->Float, Preferred->SampleRate);
        /* Pin the preferred entry to the host's channel count as well, so the engine's default
         * is the whole format and not just its rate. */
        if (Preferred->Channels != 0)
        {
            ranges[i].MaximumChannels = Preferred->Channels;
        }
        pointers[i] = (PKSDATARANGE)&ranges[i];
        i++;
    }

    for (ULONG f = 0; f < SIZEOF_ARRAY(ViosndFormatPreference); ++f)
    {
        UCHAR container, valid;
        BOOLEAN isFloat;
        if ((Caps->Formats & (1ULL << ViosndFormatPreference[f])) == 0)
        {
            continue;
        }
        if (!ViosndFormatBits(ViosndFormatPreference[f], &container, &valid, &isFloat))
        {
            continue;
        }
        for (UCHAR r = 0; r < (UCHAR)SIZEOF_ARRAY(ViosndRateTable); ++r)
        {
            if ((Caps->Rates & (1ULL << r)) == 0 || ViosndRateTable[r] == 0)
            {
                continue;
            }
            ViosndFillRange(&ranges[i], Caps, container, isFloat, ViosndRateTable[r]);
            pointers[i] = (PKSDATARANGE)&ranges[i];
            i++;
        }
    }

    *RangePointers = pointers;
    *RangeCount = i;
    return STATUS_SUCCESS;
}

VOID ViosndFreeDataRanges(_In_opt_ PKSDATARANGE *RangePointers)
{
    if (RangePointers != NULL)
    {
        ExFreePoolWithTag(RangePointers, VIOSND_POOL_TAG);
    }
}

BOOLEAN
ViosndFormatFromWave(_In_ const WAVEFORMATEX *Wfx, _In_ const VIOSND_FORMAT_CAPS *Caps, _Out_ PVIOSND_WAVE_FORMAT Out)
{
    USHORT tag = Wfx->wFormatTag;
    USHORT validBits = Wfx->wBitsPerSample;
    BOOLEAN wantFloat = FALSE;

    RtlZeroMemory(Out, sizeof(*Out));

    if (tag == WAVE_FORMAT_EXTENSIBLE)
    {
        if (Wfx->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            return FALSE;
        }
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)Wfx;
        if (IsEqualGUIDAligned(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        {
            wantFloat = TRUE;
        }
        else if (!IsEqualGUIDAligned(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        {
            return FALSE;
        }
        /* Zero means "all of the container is significant", per WAVEFORMATEXTENSIBLE. */
        if (ext->Samples.wValidBitsPerSample != 0)
        {
            validBits = ext->Samples.wValidBitsPerSample;
        }
    }
    else if (tag == WAVE_FORMAT_IEEE_FLOAT)
    {
        wantFloat = TRUE;
    }
    else if (tag != WAVE_FORMAT_PCM)
    {
        return FALSE;
    }

    if (Wfx->nChannels < Caps->ChannelsMin || Wfx->nChannels > Caps->ChannelsMax)
    {
        return FALSE;
    }

    UCHAR virtioRate;
    if (!ViosndRateFromHz(Wfx->nSamplesPerSec, &virtioRate))
    {
        return FALSE;
    }
    if ((Caps->Rates & (1ULL << virtioRate)) == 0)
    {
        return FALSE;
    }

    for (ULONG f = 0; f < SIZEOF_ARRAY(ViosndFormatPreference); ++f)
    {
        UCHAR container, valid;
        BOOLEAN isFloat;
        UCHAR candidate = ViosndFormatPreference[f];
        if ((Caps->Formats & (1ULL << candidate)) == 0)
        {
            continue;
        }
        if (!ViosndFormatBits(candidate, &container, &valid, &isFloat))
        {
            continue;
        }
        if (isFloat != wantFloat || container != Wfx->wBitsPerSample || valid != validBits)
        {
            continue;
        }
        Out->SampleRate = Wfx->nSamplesPerSec;
        Out->VirtioRate = virtioRate;
        Out->VirtioFormat = candidate;
        Out->Channels = (UCHAR)Wfx->nChannels;
        Out->ContainerBits = container;
        Out->ValidBits = valid;
        Out->Float = isFloat;
        return TRUE;
    }
    return FALSE;
}

BOOLEAN
ViosndPickPreferred(_In_ const VIOSND_FORMAT_CAPS *Caps,
                    _In_ ULONG HintRate,
                    _In_ UCHAR HintChannels,
                    _Out_ PVIOSND_WAVE_FORMAT Out)
{
    RtlZeroMemory(Out, sizeof(*Out));

    /* Format first: the widest container both sides agree on. */
    UCHAR chosenFormat = 0;
    UCHAR container = 0, valid = 0;
    BOOLEAN isFloat = FALSE;
    BOOLEAN haveFormat = FALSE;
    for (ULONG f = 0; f < SIZEOF_ARRAY(ViosndFormatPreference); ++f)
    {
        UCHAR c, v;
        BOOLEAN fl;
        if ((Caps->Formats & (1ULL << ViosndFormatPreference[f])) == 0)
        {
            continue;
        }
        if (!ViosndFormatBits(ViosndFormatPreference[f], &c, &v, &fl))
        {
            continue;
        }
        chosenFormat = ViosndFormatPreference[f];
        container = c;
        valid = v;
        isFloat = fl;
        haveFormat = TRUE;
        break;
    }
    if (!haveFormat)
    {
        return FALSE;
    }

    /* Then the rate: the host's if it can carry it, otherwise the highest at or below 48kHz --
     * above that a Windows endpoint would be defaulting to a rate nothing asked for. */
    UCHAR chosenRate = 0;
    BOOLEAN haveRate = FALSE;
    if (HintRate != 0 && ViosndRateFromHz(HintRate, &chosenRate) && (Caps->Rates & (1ULL << chosenRate)) != 0)
    {
        haveRate = TRUE;
    }
    if (!haveRate)
    {
        for (UCHAR r = 0; r < (UCHAR)SIZEOF_ARRAY(ViosndRateTable); ++r)
        {
            if ((Caps->Rates & (1ULL << r)) == 0 || ViosndRateTable[r] == 0 || ViosndRateTable[r] > 48000)
            {
                continue;
            }
            chosenRate = r;
            haveRate = TRUE;
        }
    }
    if (!haveRate)
    {
        return FALSE;
    }

    UCHAR channels = HintChannels;
    if (channels < Caps->ChannelsMin || channels > Caps->ChannelsMax)
    {
        /* No usable hint. Stereo when the stream can carry it, otherwise whatever it can. */
        channels = (Caps->ChannelsMin <= 2 && Caps->ChannelsMax >= 2) ? 2 : Caps->ChannelsMin;
    }

    Out->SampleRate = ViosndRateTable[chosenRate];
    Out->VirtioRate = chosenRate;
    Out->VirtioFormat = chosenFormat;
    Out->Channels = channels;
    Out->ContainerBits = container;
    Out->ValidBits = valid;
    Out->Float = isFloat;
    return TRUE;
}
