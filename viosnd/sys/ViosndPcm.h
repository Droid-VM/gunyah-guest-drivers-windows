#pragma once

#include "ViosndFormat.h"

#define VIOSND_DEFAULT_SAMPLE_RATE 48000u
#define VIOSND_FALLBACK_SAMPLE_RATE 16000u
#define VIOSND_DEFAULT_CHANNELS 2u
#define VIOSND_DEFAULT_BITS_PER_SAMPLE 16u
#define VIOSND_DEFAULT_PERIOD_BYTES 2048u
#define VIOSND_FALLBACK_PERIOD_BYTES 2048u
#define VIOSND_DEFAULT_BUFFER_BYTES (VIOSND_DEFAULT_PERIOD_BYTES * 16u)

/* A period is a fixed number of frames rather than a fixed number of bytes, so that its duration
 * does not change with the format and so that it is always whole frames. At the default format
 * this is exactly VIOSND_DEFAULT_PERIOD_BYTES, which is what every fixed path already used. */
#define VIOSND_PERIOD_FRAMES 512u

/* An endpoint wider than this is refused rather than carried. The width bounds a static silence
 * buffer, and virtio-snd allows a channel count far past anything a phone will route. */
#define VIOSND_MAX_CHANNELS 8u
#define VIOSND_MAX_CONTAINER_BYTES 4u
#define VIOSND_MAX_PERIOD_BYTES \
    (VIOSND_PERIOD_FRAMES * VIOSND_MAX_CHANNELS * VIOSND_MAX_CONTAINER_BYTES)

typedef struct _VIOSND_PCM_FORMAT {
    ULONG SampleRate;
    UCHAR Channels;
    UCHAR BitsPerSample;
    ULONG BufferBytes;
    ULONG PeriodBytes;
    /* What goes on the wire. Every producer of this struct fills them: rate index 0 is a real
     * rate (5512Hz), so there is no value left over to mean "unset". */
    UCHAR VirtioFormat;
    UCHAR VirtioRate;
} VIOSND_PCM_FORMAT, *PVIOSND_PCM_FORMAT;

typedef struct _VIOSND_STREAM_PAIR {
    ULONG RenderStreamId;
    ULONG CaptureStreamId;
    BOOLEAN HasRender;
    BOOLEAN HasCapture;
} VIOSND_STREAM_PAIR, *PVIOSND_STREAM_PAIR;

VOID
ViosndGetDefaultPcmFormat(
    _Out_ PVIOSND_PCM_FORMAT Format);

VOID
ViosndGetFallbackPcmFormat(
    _Out_ PVIOSND_PCM_FORMAT Format);

BOOLEAN
ViosndPcmInfoSupportsDefaultFormat(
    _In_ const VIRTIO_SND_PCM_INFO *Info);

VOID
ViosndBuildSetParams(
    _Out_ VIRTIO_SND_PCM_SET_PARAMS *Params,
    _In_ ULONG StreamId,
    _In_ const VIOSND_PCM_FORMAT *Format);

/* Turns a negotiated format into the parameters a stream is configured with. `PeriodBytes` is
 * whole frames of that format; the buffer is `NotificationCount` of them. */
VOID
ViosndPcmFormatFromWave(
    _In_ const VIOSND_WAVE_FORMAT *Wave,
    _In_ ULONG PeriodBytes,
    _In_ ULONG NotificationCount,
    _Out_ PVIOSND_PCM_FORMAT Format);

NTSTATUS
ViosndFindStreamPair(
    _In_reads_(InfoCount) const VIRTIO_SND_PCM_INFO *Info,
    _In_ ULONG InfoCount,
    _Out_ PVIOSND_STREAM_PAIR Pair);
