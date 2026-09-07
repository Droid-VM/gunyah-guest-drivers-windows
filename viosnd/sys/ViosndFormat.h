// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "VirtioSnd.h"

/*
 * Translation between what a virtio-snd PCM stream says it can do and what Windows understands.
 *
 * virtio-snd describes a stream's capability as two bitmasks plus a channel range: every format
 * and every rate it will accept, with no ordering and no way to say which one it would rather
 * have. Windows wants the opposite shape -- an explicit list of concrete formats, whose first
 * entry it treats as the one to default to. Everything here exists to turn one into the other.
 */

/* What one PCM stream reported it can do. */
typedef struct _VIOSND_FORMAT_CAPS
{
    u64 Formats; /* 1 << VIRTIO_SND_PCM_FMT_*  */
    u64 Rates;   /* 1 << VIRTIO_SND_PCM_RATE_* */
    UCHAR ChannelsMin;
    UCHAR ChannelsMax;
} VIOSND_FORMAT_CAPS, *PVIOSND_FORMAT_CAPS;

/* One concrete format, carrying both sides of the boundary so neither has to be re-derived. */
typedef struct _VIOSND_WAVE_FORMAT
{
    ULONG SampleRate;
    UCHAR VirtioRate;   /* VIRTIO_SND_PCM_RATE_*  */
    UCHAR VirtioFormat; /* VIRTIO_SND_PCM_FMT_*   */
    UCHAR Channels;
    UCHAR ContainerBits; /* WAVEFORMATEX::wBitsPerSample                     */
    UCHAR ValidBits;     /* WAVEFORMATEXTENSIBLE::Samples.wValidBitsPerSample */
    BOOLEAN Float;
} VIOSND_WAVE_FORMAT, *PVIOSND_WAVE_FORMAT;

/* Hz for a VIRTIO_SND_PCM_RATE_* index, or 0 if this driver does not know that index. */
ULONG ViosndRateHz(_In_ UCHAR VirtioRate);

/* The reverse. FALSE when the rate has no virtio-snd enumerator at all. */
BOOLEAN ViosndRateFromHz(_In_ ULONG Hz, _Out_ PUCHAR VirtioRate);

/*
 * Container/valid width for a VIRTIO_SND_PCM_FMT_*, FALSE for the ones Windows has no PCM
 * representation for (the companded and DSD formats). Note S24: virtio stores it in a four-byte
 * container with 24 valid bits, which is exactly WAVEFORMATEXTENSIBLE's 32/24 -- unlike S24_3,
 * which is packed into three.
 */
BOOLEAN ViosndFormatBits(_In_ UCHAR VirtioFormat,
                         _Out_ PUCHAR ContainerBits,
                         _Out_ PUCHAR ValidBits,
                         _Out_ PBOOLEAN Float);

/* Bytes per frame: container width times channels. */
ULONG ViosndFrameBytes(_In_ const VIOSND_WAVE_FORMAT *Format);

/*
 * Builds the pin's data range list from `Caps`.
 *
 * `Preferred` (optional) is placed first. Windows seeds an endpoint's default format from the
 * head of this list, so putting the host's own rate there is what keeps the audio engine from
 * mixing to a rate the host would only have to convert again.
 *
 * The returned pointer array and the ranges it points at are one allocation; free the whole
 * thing with ViosndFreeDataRanges.
 */
NTSTATUS ViosndBuildDataRanges(_In_ const VIOSND_FORMAT_CAPS *Caps,
                               _In_opt_ const VIOSND_WAVE_FORMAT *Preferred,
                               _Outptr_result_buffer_(*RangeCount) PKSDATARANGE **RangePointers,
                               _Out_ PULONG RangeCount);

VOID ViosndFreeDataRanges(_In_opt_ PKSDATARANGE *RangePointers);

/* Resolves a format Windows asked for against `Caps`. FALSE when the stream cannot carry it. */
BOOLEAN ViosndFormatFromWave(_In_ const WAVEFORMATEX *Wfx,
                             _In_ const VIOSND_FORMAT_CAPS *Caps,
                             _Out_ PVIOSND_WAVE_FORMAT Out);

/*
 * Chooses what to offer as the default. `HintRate`/`HintChannels` come from the host through the
 * vendor config block; zero means the host said nothing, and then this falls back to the highest
 * rate the stream supports at or below 48kHz, which is what a Windows endpoint would have
 * defaulted to anyway.
 */
BOOLEAN ViosndPickPreferred(_In_ const VIOSND_FORMAT_CAPS *Caps,
                            _In_ ULONG HintRate,
                            _In_ UCHAR HintChannels,
                            _Out_ PVIOSND_WAVE_FORMAT Out);
