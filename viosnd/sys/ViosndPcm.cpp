#include "precomp.h"

static const u64 ViosndDefaultFormatMask = (1ULL << VIRTIO_SND_PCM_FMT_S16);
static const u64 ViosndDefaultRateMask = (1ULL << VIRTIO_SND_PCM_RATE_48000);

static UCHAR
ViosndPcmRateFromSampleRate(
    _In_ ULONG SampleRate)
{
    switch (SampleRate) {
    case 16000:
        return VIRTIO_SND_PCM_RATE_16000;
    case 44100:
        return VIRTIO_SND_PCM_RATE_44100;
    case 48000:
    default:
        return VIRTIO_SND_PCM_RATE_48000;
    }
}

VOID
ViosndGetDefaultPcmFormat(
    _Out_ PVIOSND_PCM_FORMAT Format)
{
    RtlZeroMemory(Format, sizeof(*Format));
    Format->SampleRate = VIOSND_DEFAULT_SAMPLE_RATE;
    Format->Channels = VIOSND_DEFAULT_CHANNELS;
    Format->BitsPerSample = VIOSND_DEFAULT_BITS_PER_SAMPLE;
    Format->BufferBytes = VIOSND_DEFAULT_BUFFER_BYTES;
    Format->PeriodBytes = VIOSND_DEFAULT_PERIOD_BYTES;
    Format->VirtioFormat = VIRTIO_SND_PCM_FMT_S16;
    Format->VirtioRate = ViosndPcmRateFromSampleRate(VIOSND_DEFAULT_SAMPLE_RATE);
}

VOID
ViosndGetFallbackPcmFormat(
    _Out_ PVIOSND_PCM_FORMAT Format)
{
    ViosndGetDefaultPcmFormat(Format);
    Format->SampleRate = VIOSND_FALLBACK_SAMPLE_RATE;
    Format->VirtioRate = ViosndPcmRateFromSampleRate(VIOSND_FALLBACK_SAMPLE_RATE);
    Format->PeriodBytes = VIOSND_FALLBACK_PERIOD_BYTES;
    Format->BufferBytes = VIOSND_FALLBACK_PERIOD_BYTES * 16u;
}

BOOLEAN
ViosndPcmInfoSupportsDefaultFormat(
    _In_ const VIRTIO_SND_PCM_INFO *Info)
{
    if (Info == NULL) {
        return FALSE;
    }

    if ((Info->formats & ViosndDefaultFormatMask) == 0) {
        return FALSE;
    }

    if ((Info->rates & ViosndDefaultRateMask) == 0) {
        return FALSE;
    }

    return Info->channels_min <= VIOSND_DEFAULT_CHANNELS &&
           Info->channels_max >= VIOSND_DEFAULT_CHANNELS;
}

VOID
ViosndBuildSetParams(
    _Out_ VIRTIO_SND_PCM_SET_PARAMS *Params,
    _In_ ULONG StreamId,
    _In_ const VIOSND_PCM_FORMAT *Format)
{
    RtlZeroMemory(Params, sizeof(*Params));
    Params->hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    Params->hdr.stream_id = StreamId;
    Params->buffer_bytes = Format->BufferBytes;
    Params->period_bytes = Format->PeriodBytes;
    Params->features = 0;
    Params->channels = Format->Channels;
    Params->format = Format->VirtioFormat;
    Params->rate = Format->VirtioRate;
}

VOID
ViosndPcmFormatFromWave(
    _In_ const VIOSND_WAVE_FORMAT *Wave,
    _In_ ULONG PeriodBytes,
    _In_ ULONG NotificationCount,
    _Out_ PVIOSND_PCM_FORMAT Format)
{
    ULONG frameBytes = ViosndFrameBytes(Wave);

    RtlZeroMemory(Format, sizeof(*Format));
    Format->SampleRate = Wave->SampleRate;
    Format->Channels = Wave->Channels;
    Format->BitsPerSample = Wave->ContainerBits;
    Format->VirtioFormat = Wave->VirtioFormat;
    Format->VirtioRate = Wave->VirtioRate;

    /* A period that is not whole frames desynchronises every position the driver reports, so
     * round down and let the buffer follow from what the period actually became. */
    if (frameBytes == 0) {
        frameBytes = 1;
    }
    if (PeriodBytes < frameBytes) {
        PeriodBytes = frameBytes;
    }
    Format->PeriodBytes = (PeriodBytes / frameBytes) * frameBytes;
    if (NotificationCount == 0) {
        NotificationCount = 1;
    }
    Format->BufferBytes = Format->PeriodBytes * NotificationCount;
}

NTSTATUS
ViosndFindStreamPair(
    _In_reads_(InfoCount) const VIRTIO_SND_PCM_INFO *Info,
    _In_ ULONG InfoCount,
    _Out_ PVIOSND_STREAM_PAIR Pair)
{
    ULONG fallbackRender = MAXULONG;
    ULONG fallbackCapture = MAXULONG;

    RtlZeroMemory(Pair, sizeof(*Pair));

    for (ULONG i = 0; i < InfoCount; ++i) {
        if (Info[i].direction == VIRTIO_SND_D_OUTPUT && fallbackRender == MAXULONG) {
            fallbackRender = i;
        } else if (Info[i].direction == VIRTIO_SND_D_INPUT && fallbackCapture == MAXULONG) {
            fallbackCapture = i;
        }

        if (!ViosndPcmInfoSupportsDefaultFormat(&Info[i])) {
            continue;
        }

        if (!Pair->HasRender && Info[i].direction == VIRTIO_SND_D_OUTPUT) {
            Pair->RenderStreamId = i;
            Pair->HasRender = TRUE;
        } else if (!Pair->HasCapture && Info[i].direction == VIRTIO_SND_D_INPUT) {
            Pair->CaptureStreamId = i;
            Pair->HasCapture = TRUE;
        }

        if (Pair->HasRender && Pair->HasCapture) {
            return STATUS_SUCCESS;
        }
    }

    if (!Pair->HasRender && fallbackRender != MAXULONG) {
        Pair->RenderStreamId = fallbackRender;
        Pair->HasRender = TRUE;
    }

    if (!Pair->HasCapture && fallbackCapture != MAXULONG) {
        Pair->CaptureStreamId = fallbackCapture;
        Pair->HasCapture = TRUE;
    }

    // Deliberately no fallback that invents a capture stream out of a spare index: a card that
    // reports only output streams has only output streams, and pointing a capture endpoint at an
    // output stream produces an endpoint that can never record. Report what the device has and
    // let the caller decide what it can expose.

    return Pair->HasRender || Pair->HasCapture ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}
