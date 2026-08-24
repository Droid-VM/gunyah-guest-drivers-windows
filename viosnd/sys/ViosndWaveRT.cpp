#include "precomp.h"

enum {
    VIOSND_NODE_DAC_ADC = 0
};

/*
 * The deepest queue the host can ask for, and so the size of both IO pools.
 *
 * The two used to be written separately -- 12 for render, 8 for capture -- and the capture one
 * was below the deepest setting the host offers. Choosing it gave the speaker the depth asked
 * for and the microphone whatever fitted, with nothing to say the two had been treated
 * differently. Deriving both from one number is what stops that drifting apart again.
 */
#define VIOSND_MAX_OUTSTANDING_PACKETS 12u

#define VIOSND_RENDER_IO_POOL_SIZE VIOSND_MAX_OUTSTANDING_PACKETS
#define VIOSND_RENDER_TARGET_OUTSTANDING_PACKETS 8u
/* How much audio the render pump keeps in flight. Two packets is ~21ms at the
 * default 2048-byte period, and anything that keeps the pump off the CPU for
 * longer than that is an underrun: crosvm finds the TX queue empty, writes a
 * period of silence, and that gap is the click. On a Gunyah pVM the guest has no
 * display driver at all -- DWM composites and video decodes in software -- so
 * 21ms scheduling gaps are ordinary, and the clicks track content complexity
 * rather than anything in the audio path (which carries raw PCM end to end).
 * Six packets is ~64ms of slack, at the cost of ~40ms more latency, which the OS
 * hides through the audio clock position. Still bounded by NotificationCount - 1:
 * the pump must never send a packet the OS has not written yet. */
#define VIOSND_RENDER_CYCLIC_OUTSTANDING_PACKETS 6u
#define VIOSND_RENDER_FALLBACK_OUTSTANDING_PACKETS 6u
#define VIOSND_RENDER_START_PREROLL_PACKETS 2u
#define VIOSND_RENDER_CYCLIC_PREROLL_PACKETS VIOSND_RENDER_START_PREROLL_PACKETS
#define VIOSND_CAPTURE_IO_POOL_SIZE VIOSND_MAX_OUTSTANDING_PACKETS
#define VIOSND_CAPTURE_TARGET_OUTSTANDING_PACKETS 4u
#define VIOSND_COMPLETION_STALL_LOG_LOOPS 50u
#define VIOSND_RENDER_REKICK_STALL_LOOPS 5u
#define VIOSND_RENDER_POLL_INTERVAL_US 1000LL
#define VIOSND_CAPTURE_POLL_INTERVAL_US 1000LL
#define VIOSND_CAPTURE_STOP_RECLAIM_POLLS 2u
#define VIOSND_CAPTURE_STOP_TIMEOUT_MS 50LL
#define VIOSND_RENDER_STOP_TIMEOUT_MS 250LL
#define VIOSND_CAPTURE_SOFTWARE_GAIN 1
#define VIOSND_CAPTURE_NOISE_GATE_PEAK 0u
#define VIOSND_PERIODIC_LOG_MASK 0x1ffu
#define VIOSND_RENDER_SOFTWARE_CLOCK 1
#define VIOSND_FALLBACK_MONITOR_MS 2000LL
#define VIOSND_FALLBACK_TRIGGER_PERCENT 65u

static volatile LONG ViosndCaptureFaulted;
static UCHAR ViosndSilencePeriod[VIOSND_MAX_PERIOD_BYTES];

static KSDATARANGE_AUDIO ViosndPinDataRangesStream[] = {
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        VIOSND_DEFAULT_CHANNELS,
        VIOSND_DEFAULT_BITS_PER_SAMPLE,
        VIOSND_DEFAULT_BITS_PER_SAMPLE,
        VIOSND_DEFAULT_SAMPLE_RATE,
        VIOSND_DEFAULT_SAMPLE_RATE
    }
};

static PKSDATARANGE ViosndPinDataRangePointersStream[] = {
    PKSDATARANGE(&ViosndPinDataRangesStream[0])
};

static KSDATARANGE ViosndPinDataRangesBridge[] = {
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE ViosndPinDataRangePointersBridge[] = {
    &ViosndPinDataRangesBridge[0]
};

static PCPIN_DESCRIPTOR ViosndRenderPins[] = {
    {
        1,
        1,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersStream),
            ViosndPinDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersBridge),
            ViosndPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    }
};

static PCNODE_DESCRIPTOR ViosndRenderNodes[] = {
    { 0, NULL, &KSNODETYPE_DAC, NULL }
};

static PCCONNECTION_DESCRIPTOR ViosndRenderConnections[] = {
    { PCFILTER_NODE, VIOSND_PIN_SYSTEM, VIOSND_NODE_DAC_ADC, 1 },
    { VIOSND_NODE_DAC_ADC, 0, PCFILTER_NODE, VIOSND_PIN_BRIDGE }
};

static PCFILTER_DESCRIPTOR ViosndRenderFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndRenderPins),
    ViosndRenderPins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndRenderNodes),
    ViosndRenderNodes,
    SIZEOF_ARRAY(ViosndRenderConnections),
    ViosndRenderConnections,
    0,
    NULL
};

static PCPIN_DESCRIPTOR ViosndCapturePins[] = {
    {
        1,
        1,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersStream),
            ViosndPinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(ViosndPinDataRangePointersBridge),
            ViosndPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    }
};

static PCNODE_DESCRIPTOR ViosndCaptureNodes[] = {
    { 0, NULL, &KSNODETYPE_ADC, NULL }
};

static PCCONNECTION_DESCRIPTOR ViosndCaptureConnections[] = {
    { PCFILTER_NODE, VIOSND_PIN_BRIDGE, VIOSND_NODE_DAC_ADC, 1 },
    { VIOSND_NODE_DAC_ADC, 0, PCFILTER_NODE, VIOSND_PIN_SYSTEM }
};

static PCFILTER_DESCRIPTOR ViosndCaptureFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndCapturePins),
    ViosndCapturePins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(ViosndCaptureNodes),
    ViosndCaptureNodes,
    SIZEOF_ARRAY(ViosndCaptureConnections),
    ViosndCaptureConnections,
    0,
    NULL
};

/* A period is whole frames of the stream's own format, and the same duration whatever that
 * format is. Clamped so it always fits the static silence buffer. */
static ULONG
ViosndPeriodBytesForFormat(
    _In_ const VIOSND_WAVE_FORMAT *Format)
{
    ULONG frameBytes = ViosndFrameBytes(Format);
    ULONG period;

    if (frameBytes == 0) {
        return VIOSND_DEFAULT_PERIOD_BYTES;
    }
    period = VIOSND_PERIOD_FRAMES * frameBytes;
    if (period > VIOSND_MAX_PERIOD_BYTES) {
        period = (VIOSND_MAX_PERIOD_BYTES / frameBytes) * frameBytes;
    }
    return period != 0 ? period : VIOSND_DEFAULT_PERIOD_BYTES;
}

/* The channel mask Windows expects beside a channel count. Anything past the ones with a named
 * layout gets the first N channels, which is what KSAUDIO's own tables do. */
static ULONG
ViosndChannelMask(
    _In_ UCHAR Channels)
{
    switch (Channels) {
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default:
        return 0;
    }
}

/* Renders a negotiated format into the shape the port driver hands around. */
static VOID
ViosndBuildWaveFormat(
    _In_ const VIOSND_WAVE_FORMAT *Wave,
    _Out_ PKSDATAFORMAT_WAVEFORMATEXTENSIBLE Format)
{
    RtlZeroMemory(Format, sizeof(*Format));
    Format->DataFormat.FormatSize = sizeof(*Format);
    Format->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    Format->DataFormat.SubFormat =
        Wave->Float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    Format->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    Format->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    Format->WaveFormatExt.Format.nChannels = Wave->Channels;
    Format->WaveFormatExt.Format.nSamplesPerSec = Wave->SampleRate;
    Format->WaveFormatExt.Format.wBitsPerSample = Wave->ContainerBits;
    Format->WaveFormatExt.Format.nBlockAlign = (USHORT)ViosndFrameBytes(Wave);
    Format->WaveFormatExt.Format.nAvgBytesPerSec =
        Wave->SampleRate * Format->WaveFormatExt.Format.nBlockAlign;
    Format->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    Format->WaveFormatExt.Samples.wValidBitsPerSample = Wave->ValidBits;
    Format->WaveFormatExt.dwChannelMask = ViosndChannelMask(Wave->Channels);
    Format->WaveFormatExt.SubFormat =
        Wave->Float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

/* Resolves what the port asked for against what the stream can carry. */
static BOOLEAN
ViosndResolveDataFormat(
    _In_opt_ PKSDATAFORMAT DataFormat,
    _In_ const VIOSND_FORMAT_CAPS *Caps,
    _Out_ PVIOSND_WAVE_FORMAT Out)
{
    RtlZeroMemory(Out, sizeof(*Out));

    if (DataFormat == NULL ||
        !IsEqualGUIDAligned(DataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(DataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
        DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        return FALSE;
    }

    return ViosndFormatFromWave(&((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx,
                                Caps,
                                Out);
}

static ULONG
ViosndTargetOutstandingPackets(
    _In_ ULONG NotificationCount)
{
    if (NotificationCount == 0) {
        return 0;
    }

    if (NotificationCount == 1) {
        return 1;
    }

    return min(NotificationCount - 1, VIOSND_RENDER_TARGET_OUTSTANDING_PACKETS);
}

static ULONG
ViosndCyclicTargetOutstandingPackets(
    _In_ ULONG NotificationCount)
{
    if (NotificationCount == 0) {
        return 0;
    }

    if (NotificationCount == 1) {
        return 1;
    }

    return min(NotificationCount - 1, VIOSND_RENDER_CYCLIC_OUTSTANDING_PACKETS);
}

static ULONG
ViosndFallbackTargetOutstandingPackets(
    _In_ ULONG NotificationCount)
{
    if (NotificationCount == 0) {
        return 0;
    }

    if (NotificationCount == 1) {
        return 1;
    }

    return min(NotificationCount - 1, VIOSND_RENDER_FALLBACK_OUTSTANDING_PACKETS);
}

static ULONG
ViosndFallbackInitialPhase()
{
    return VIOSND_DEFAULT_SAMPLE_RATE - VIOSND_FALLBACK_SAMPLE_RATE;
}

static BOOLEAN
ViosndPacketNumberLessOrEqual(
    _In_ ULONG Left,
    _In_ ULONG Right)
{
    return (LONG)(Left - Right) <= 0;
}

static BOOLEAN
ViosndHasWritableRenderPacket(
    _In_ ULONG NextSubmitPacket,
    _In_ ULONG LastOsWritePacket)
{
    return LastOsWritePacket != MAXULONG &&
           ViosndPacketNumberLessOrEqual(NextSubmitPacket, LastOsWritePacket);
}

/* Whole periods of the stream's own format. At the default format PeriodBytes is 2048 and all
 * three of these behave exactly as they did when that number was written into them. */
static ULONG
ViosndPacketSizeFromNotificationCount(
    _In_ ULONG BufferSize,
    _In_ ULONG NotificationCount,
    _In_ ULONG PeriodBytes)
{
    UNREFERENCED_PARAMETER(NotificationCount);

    if (BufferSize < PeriodBytes) {
        return BufferSize;
    }

    return PeriodBytes;
}

static ULONG
ViosndNotificationCountFromBufferSize(
    _In_ ULONG BufferSize,
    _In_ ULONG PeriodBytes)
{
    if (BufferSize == 0 || PeriodBytes == 0) {
        return 0;
    }

    return max(BufferSize / PeriodBytes, 1u);
}

static ULONG
ViosndUsableBufferSizeFromAllocatedSize(
    _In_ ULONG AllocatedSize,
    _In_ ULONG PeriodBytes)
{
    if (PeriodBytes == 0 || AllocatedSize < PeriodBytes) {
        return AllocatedSize;
    }

    ULONG notificationCount = ViosndNotificationCountFromBufferSize(AllocatedSize, PeriodBytes);

    return notificationCount * PeriodBytes;
}

static ULONGLONG
ViosndRenderBytesFromQpc(
    _In_ LONGLONG QpcDelta,
    _In_ LONGLONG QpcFrequency,
    _In_ ULONG SampleRate,
    _In_ ULONG FrameBytes)
{
    /* The stream's own rate and frame size, not the default ones. This is the position the
     * render pin reports, and the audio engine paces every write it makes by it: describing a
     * 32-bit stream with a 16-bit frame size advances it at half speed, and what comes out is
     * not quiet or late but torn. */
    const ULONGLONG bytesPerSecond = (ULONGLONG)SampleRate * FrameBytes;
    const ULONGLONG blockAlign = FrameBytes;
    ULONGLONG bytes;

    if (QpcDelta <= 0 || QpcFrequency <= 0 || bytesPerSecond == 0 || blockAlign == 0) {
        return 0;
    }

    bytes = ((ULONGLONG)QpcDelta * bytesPerSecond) / (ULONGLONG)QpcFrequency;
    return bytes - (bytes % blockAlign);
}

static ULONG
ViosndPcmPeak16(
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    const SHORT *samples = (const SHORT *)Buffer;
    ULONG sampleCount = Length / sizeof(SHORT);
    ULONG peak = 0;

    for (ULONG i = 0; i < sampleCount; ++i) {
        LONG sample = samples[i];
        ULONG magnitude = sample < 0 ? (ULONG)-sample : (ULONG)sample;

        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    return peak;
}

static ULONG
ViosndPcmCopyGain16(
    _Out_writes_bytes_(Length) VOID *Destination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ ULONG Length,
    _In_ LONG Gain)
{
    SHORT *destination = (SHORT *)Destination;
    const SHORT *source = (const SHORT *)Source;
    ULONG sampleCount = Length / sizeof(SHORT);
    ULONG peak = 0;

    for (ULONG i = 0; i < sampleCount; ++i) {
        LONG sample = (LONG)source[i] * Gain;

        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }

        destination[i] = (SHORT)sample;

        ULONG magnitude = sample < 0 ? (ULONG)-sample : (ULONG)sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    if ((Length & 1u) != 0) {
        ((PUCHAR)Destination)[Length - 1] = ((const UCHAR *)Source)[Length - 1];
    }

    return peak;
}

static ULONG
ViosndDownsampleStereo16(
    _Out_writes_bytes_(DestinationLength) VOID *Destination,
    _In_ ULONG DestinationLength,
    _In_reads_bytes_(SourceLength) const VOID *Source,
    _In_ ULONG SourceLength,
    _In_ ULONG DestinationSampleRate,
    _Inout_ PULONG Phase)
{
    SHORT *destination = (SHORT *)Destination;
    const SHORT *source = (const SHORT *)Source;
    ULONG sourceFrames = SourceLength /
                         (VIOSND_DEFAULT_CHANNELS * sizeof(SHORT));
    ULONG maxDestinationFrames = DestinationLength /
                                 (VIOSND_DEFAULT_CHANNELS * sizeof(SHORT));
    ULONG outputFrames = 0;
    ULONG phase = *Phase;

    if (DestinationSampleRate == 0 ||
        DestinationSampleRate >= VIOSND_DEFAULT_SAMPLE_RATE) {
        return 0;
    }

    for (ULONG frame = 0; frame < sourceFrames && outputFrames < maxDestinationFrames; ++frame) {
        phase += DestinationSampleRate;
        if (phase >= VIOSND_DEFAULT_SAMPLE_RATE) {
            phase -= VIOSND_DEFAULT_SAMPLE_RATE;
            destination[(outputFrames * 2u)] = source[(frame * 2u)];
            destination[(outputFrames * 2u) + 1u] = source[(frame * 2u) + 1u];
            outputFrames++;
        }
    }

    *Phase = phase;
    return outputFrames * VIOSND_DEFAULT_CHANNELS * sizeof(SHORT);
}

static VOID
ViosndRenderThread(_In_ PVOID Context);

class CViosndMiniportWaveRT;

class CViosndMiniportWaveRTStream :
    public IMiniportWaveRTStreamNotification,
    public IMiniportWaveRTInputStream,
    public IMiniportWaveRTOutputStream
{
    friend VOID ViosndRenderThread(_In_ PVOID Context);

public:
    CViosndMiniportWaveRTStream(
        _In_ CViosndMiniportWaveRT *Miniport,
        _In_ PVIOSND_DEVICE Device,
        _In_ ULONG StreamId,
        _In_ BOOLEAN Capture,
        _In_ const VIOSND_WAVE_FORMAT *Format);

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTInputStream;
    IMP_IMiniportWaveRTOutputStream;

    STDMETHODIMP_(NTSTATUS) QueryInterface(_In_ REFIID InterfaceId, _COM_Outptr_ PVOID *Interface);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

private:
    LONG m_RefCount;
    CViosndMiniportWaveRT *m_Miniport;
    PVIOSND_DEVICE m_Device;
    ULONG m_StreamId;
    BOOLEAN m_Capture;
    KSSTATE m_State;
    PMDL m_Mdl;
    PVOID m_Buffer;
    ULONG m_BufferSize;
    ULONG m_NotificationCount;
    ULONG m_PacketSize;
    ULONG m_PacketNumber;
    ULONG m_LastOsWritePacket;
    ULONG m_NextSubmitPacket;
    ULONG m_NextReadPacket;
    ULONG m_EosPacketNumber;
    ULONG m_EosPacketLength;
    ULONG m_OutstandingWrites;
    ULONG m_RenderUnderruns;
    ULONG m_RenderFallbackPackets;
    ULONG m_RenderPrerollPackets;
    ULONG m_CaptureInFlight;
    ULONG m_CaptureOverflows;
    ULONG m_RenderStallLoops;
    ULONG m_CaptureStallLoops;
    ULONG m_RenderPositionQueries;
    ULONG m_CapturePositionQueries;
    ULONG m_CaptureReadPackets;
    ULONG m_CaptureSubmitOk;
    /* Loudest sample seen arriving from the host, and after the driver's own copy. The two
     * separate "the host is sending silence" from "the guest is not delivering what arrived". */
    ULONG m_CaptureMaxInputPeak;
    ULONG m_CaptureMaxOutputPeak;
    ULONG m_CaptureBytesCopied;
    ULONG m_CaptureSubmitFail;
    NTSTATUS m_CaptureLastSubmitStatus;
    ULONG m_RenderFallbackPhase;
    PVIOSND_PCM_IO m_RenderIoPool[VIOSND_RENDER_IO_POOL_SIZE];
    ULONG m_RenderIoFreeCount;
    PVOID m_RenderFallbackBuffer;
    ULONG m_RenderFallbackBufferSize;
    PVIOSND_PCM_IO m_CaptureIoPool[VIOSND_CAPTURE_IO_POOL_SIZE];
    ULONG m_CaptureIoFreeCount;
    ULONGLONG m_Position;
    ULONGLONG m_RunStartPosition;
    LONGLONG m_RunStartQpc;
    /* When the endpoint last took a period. The reported position is measured forward from this
     * rather than from the start of the run, so it can never drift away from what was consumed. */
    LONGLONG m_LastCompletionQpc;
    LONGLONG m_QpcFrequency;
    ULONG m_RenderNotReadyLoops;
    ULONGLONG m_FallbackMonitorStartPosition;
    LONGLONG m_FallbackMonitorStartQpc;
    PKEVENT m_NotificationEvent;
    KEVENT m_StopEvent;
    KEVENT m_KickEvent;
    PVOID m_ThreadObject;
    BOOLEAN m_WorkerStarted;
    BOOLEAN m_RenderFallbackActive;
    BOOLEAN m_RenderFallbackAttempted;
    /* The format this stream negotiated, and the period in bytes that follows from it. Every
     * buffer size, packet size and SET_PARAMS on this stream is derived from these two. */
    VIOSND_WAVE_FORMAT m_WaveFormat;
    ULONG m_PeriodBytes;

    NTSTATUS ConfigureNegotiatedPcm();

    NTSTATUS SubmitRenderPacket(_In_ ULONG PacketNumber, _In_ ULONG PacketLength);
    NTSTATUS SubmitRenderSilencePacket(_In_ ULONG PacketNumber);
    VOID ReclaimRenderPackets(_Inout_ PULONG SubmittedSinceLog);
    VOID CancelRenderWrites(_In_ NTSTATUS FailureStatus);
    NTSTATUS AllocateRenderIoPool();
    VOID FreeRenderIoPool();
    NTSTATUS AllocateCaptureIoPool();
    VOID FreeCaptureIoPool();
    NTSTATUS SubmitCapturePacket();
    VOID ReclaimCapturePackets();
    NTSTATUS StartRenderWorker();
    BOOLEAN StopRenderWorker();
    ULONGLONG GetRenderClockPosition();
    VOID MaybeSwitchRenderFallback();
    NTSTATUS SwitchRenderToFallback();
    VOID RenderWorkerLoop();
    VOID CaptureWorkerLoop();
};

class CViosndMiniportWaveRT : public IMiniportWaveRT
{
public:
    CViosndMiniportWaveRT(_In_ PVIOSND_DEVICE Device, _In_ const VIOSND_ENDPOINT *Endpoint);

    IMP_IMiniportWaveRT;

    STDMETHODIMP_(NTSTATUS) QueryInterface(_In_ REFIID InterfaceId, _COM_Outptr_ PVOID *Interface);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    BOOLEAN IsCapture() const { return m_Capture; }
    const VIOSND_FORMAT_CAPS *Caps() const { return &m_Endpoint.Caps; }
    const VIOSND_WAVE_FORMAT *Preferred() const { return &m_Endpoint.Preferred; }

    /* Builds the filter this endpoint will show. Separate from the constructor because it
     * allocates, and a constructor has nowhere to put the failure. */
    NTSTATUS BuildDescription();

private:
    LONG m_RefCount;
    PVIOSND_DEVICE m_Device;
    ULONG m_StreamId;
    BOOLEAN m_Capture;
    PPORTWAVERT m_Port;
    /* What the host said this endpoint is, and what it can carry. */
    VIOSND_ENDPOINT m_Endpoint;
    /* The pin's data ranges, built from the endpoint's caps. One allocation, owned here for as
     * long as the filter that points at it. */
    PKSDATARANGE *m_RangePointers;
    ULONG m_RangeCount;
    /* A per-instance copy of the static description, so two endpoints on one device can offer
     * different formats. The static tables stay as the template. */
    PCPIN_DESCRIPTOR m_Pins[2];
    PCFILTER_DESCRIPTOR m_Filter;
};

CViosndMiniportWaveRTStream::CViosndMiniportWaveRTStream(
    _In_ CViosndMiniportWaveRT *Miniport,
    _In_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ BOOLEAN Capture,
    _In_ const VIOSND_WAVE_FORMAT *Format) :
    m_RefCount(1),
    m_Miniport(Miniport),
    m_Device(Device),
    m_StreamId(StreamId),
    m_Capture(Capture),
    m_State(KSSTATE_STOP),
    m_Mdl(NULL),
    m_Buffer(NULL),
    m_BufferSize(0),
    m_NotificationCount(0),
    m_PacketSize(0),
    m_PacketNumber(0),
    m_LastOsWritePacket(MAXULONG),
    m_NextSubmitPacket(0),
    m_NextReadPacket(0),
    m_EosPacketNumber(MAXULONG),
    m_EosPacketLength(0),
    m_OutstandingWrites(0),
    m_RenderUnderruns(0),
    m_RenderFallbackPackets(0),
    m_RenderPrerollPackets(0),
    m_CaptureInFlight(0),
    m_CaptureOverflows(0),
    m_RenderStallLoops(0),
    m_CaptureStallLoops(0),
    m_RenderPositionQueries(0),
    m_CapturePositionQueries(0),
    m_CaptureReadPackets(0),
    m_CaptureSubmitOk(0),
    m_CaptureMaxInputPeak(0),
    m_CaptureMaxOutputPeak(0),
    m_CaptureBytesCopied(0),
    m_CaptureSubmitFail(0),
    m_CaptureLastSubmitStatus(STATUS_SUCCESS),
    m_RenderFallbackPhase(0),
    m_RenderIoFreeCount(0),
    m_RenderFallbackBuffer(NULL),
    m_RenderFallbackBufferSize(0),
    m_CaptureIoFreeCount(0),
    m_Position(0),
    m_RunStartPosition(0),
    m_RunStartQpc(0),
    m_LastCompletionQpc(0),
    m_QpcFrequency(0),
    m_RenderNotReadyLoops(0),
    m_FallbackMonitorStartPosition(0),
    m_FallbackMonitorStartQpc(0),
    m_NotificationEvent(NULL),
    m_ThreadObject(NULL),
    m_WorkerStarted(FALSE),
    m_RenderFallbackActive(FALSE),
    m_RenderFallbackAttempted(FALSE),
    m_PeriodBytes(0)
{
    m_WaveFormat = *Format;
    m_PeriodBytes = ViosndPeriodBytesForFormat(&m_WaveFormat);
    KeInitializeEvent(&m_StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&m_KickEvent, SynchronizationEvent, FALSE);
    RtlZeroMemory(m_RenderIoPool, sizeof(m_RenderIoPool));
    RtlZeroMemory(m_CaptureIoPool, sizeof(m_CaptureIoPool));
    m_Miniport->AddRef();
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::QueryInterface(
    _In_ REFIID InterfaceId,
    _COM_Outptr_ PVOID *Interface)
{
    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTStream) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTStreamNotification)) {
        *Interface = (IMiniportWaveRTStreamNotification *)this;
    } else if (m_Capture && IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTInputStream)) {
        *Interface = (IMiniportWaveRTInputStream *)this;
    } else if (!m_Capture && IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRTOutputStream)) {
        *Interface = (IMiniportWaveRTOutputStream *)this;
    }

    if (*Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRTStream::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRTStream::Release()
{
    if (m_ThreadObject != NULL && PsGetCurrentThread() != m_ThreadObject) {
        StopRenderWorker();
    }

    LONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        if (m_ThreadObject != NULL && PsGetCurrentThread() == m_ThreadObject) {
            ObDereferenceObject(m_ThreadObject);
            m_ThreadObject = NULL;
            m_WorkerStarted = FALSE;
        } else {
            StopRenderWorker();
        }
        if (m_Mdl != NULL) {
            FreeAudioBuffer(m_Mdl, m_BufferSize);
        }
        FreeRenderIoPool();
        FreeCaptureIoPool();
        m_Miniport->Release();
        this->~CViosndMiniportWaveRTStream();
        ExFreePoolWithTag(this, VIOSND_POOL_TAG);
    }
    return (ULONG)count;
}

NTSTATUS
CViosndMiniportWaveRTStream::ConfigureNegotiatedPcm()
{
    VIOSND_PCM_FORMAT format;

    ViosndPcmFormatFromWave(&m_WaveFormat,
                            m_PeriodBytes,
                            m_NotificationCount != 0 ? m_NotificationCount : 1,
                            &format);
    return ViosndConfigurePcm(m_Device, m_StreamId, &format);
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat)
{
    VIOSND_WAVE_FORMAT wave;

    if (!ViosndResolveDataFormat(DataFormat, m_Miniport->Caps(), &wave)) {
        return STATUS_NO_MATCH;
    }

    /* The period follows from the format, and the buffer was cut into periods when it was
     * allocated. Changing one without the other would leave every position this stream reports
     * describing a buffer that is no longer there, so a format change after allocation is only
     * allowed when it changes nothing. */
    if (m_Buffer != NULL && RtlCompareMemory(&wave, &m_WaveFormat, sizeof(wave)) != sizeof(wave)) {
        return STATUS_NO_MATCH;
    }

    m_WaveFormat = wave;
    m_PeriodBytes = ViosndPeriodBytesForFormat(&wave);
    return STATUS_SUCCESS;
}

NTSTATUS
CViosndMiniportWaveRTStream::SubmitRenderPacket(
    _In_ ULONG PacketNumber,
    _In_ ULONG PacketLength)
{
    ULONG offset;
    ULONG length;
    PUCHAR packet;
    const VOID *submitPacket;
    ULONG submitLength;
    ULONG sourceLength;
    ULONG sourcePackets;
    NTSTATUS status;
    PVIOSND_PCM_IO io;

    if (m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (m_RenderIoFreeCount == 0) {
        return STATUS_DEVICE_BUSY;
    }

    length = min(PacketLength, m_PacketSize);
    offset = (PacketNumber % max(m_NotificationCount, 1u)) * m_PacketSize;
    packet = (PUCHAR)m_Buffer + offset;
    KeMemoryBarrier();
    if (PacketLength < m_PacketSize) {
        RtlZeroMemory(packet + length, m_PacketSize - length);
        length = m_PacketSize;
    }
    submitPacket = packet;
    submitLength = length;
    sourceLength = length;
    sourcePackets = 1;

    if (m_RenderFallbackActive) {
        ULONG fallbackPhase = m_RenderFallbackPhase;

        if (m_RenderFallbackBuffer == NULL ||
            m_RenderFallbackBufferSize < VIOSND_FALLBACK_PERIOD_BYTES) {
            return STATUS_DEVICE_NOT_READY;
        }
        submitLength = 0;
        sourceLength = 0;
        sourcePackets = 0;
        for (ULONG i = 0; i < 3u && submitLength < VIOSND_FALLBACK_PERIOD_BYTES; ++i) {
            ULONG sourcePacket = PacketNumber + i;
            ULONG sourceOffset;
            PUCHAR source;
            ULONG sourcePacketLength = m_PacketSize;
            ULONG downsampled;

            if (m_LastOsWritePacket != MAXULONG &&
                !ViosndPacketNumberLessOrEqual(sourcePacket, m_LastOsWritePacket)) {
                break;
            }

            if (m_EosPacketNumber == sourcePacket) {
                sourcePacketLength = m_EosPacketLength;
            }

            sourceOffset = (sourcePacket % max(m_NotificationCount, 1u)) * m_PacketSize;
            source = (PUCHAR)m_Buffer + sourceOffset;
            downsampled = ViosndDownsampleStereo16((PUCHAR)m_RenderFallbackBuffer + submitLength,
                                                   VIOSND_FALLBACK_PERIOD_BYTES - submitLength,
                                                   source,
                                                   sourcePacketLength,
                                                   VIOSND_FALLBACK_SAMPLE_RATE,
                                                   &fallbackPhase);
            submitLength += downsampled;
            sourceLength += sourcePacketLength;
            sourcePackets++;

            if (m_EosPacketNumber == sourcePacket) {
                break;
            }
        }
        submitPacket = m_RenderFallbackBuffer;
        if (submitLength == 0) {
            return STATUS_INVALID_BUFFER_SIZE;
        }
        m_RenderFallbackPhase = fallbackPhase;
    }

    io = m_RenderIoPool[--m_RenderIoFreeCount];
    m_RenderIoPool[m_RenderIoFreeCount] = NULL;

    if ((PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0 ||
        PacketLength < m_PacketSize) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render submit begin packet=%u length=%u requested=%u offset=%u outstanding=%u free=%u last=%u\n",
                   m_StreamId,
                   PacketNumber,
                   submitLength,
                   PacketLength,
                   offset,
                   m_OutstandingWrites,
                   m_RenderIoFreeCount,
                   m_LastOsWritePacket);
    }

    status = ViosndSubmitPreparedWritePcm(m_Device,
                                          m_StreamId,
                                          submitPacket,
                                          submitLength,
                                          sourceLength,
                                          PacketNumber,
                                          io);
    if (NT_SUCCESS(status)) {
        m_NextSubmitPacket = PacketNumber + sourcePackets;
        m_OutstandingWrites++;
        if ((PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0 ||
            PacketLength < m_PacketSize) {
            ULONG peak = ViosndPcmPeak16(submitPacket, submitLength);
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render submit packet=%u length=%u source=%u packets=%u requested=%u peak=%u offset=%u outstanding=%u last=%u eos=%u/%u fallback=%u\n",
                       m_StreamId,
                       PacketNumber,
                       submitLength,
                       sourceLength,
                       sourcePackets,
                       PacketLength,
                       peak,
                       offset,
                       m_OutstandingWrites,
                       m_LastOsWritePacket,
                       m_EosPacketNumber,
                       m_EosPacketLength,
                       m_RenderFallbackActive);
        }
    } else {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u submit render packet=%u length=%u failed 0x%08x\n",
                   m_StreamId,
                   PacketNumber,
                   submitLength,
                   status);
        m_RenderIoPool[m_RenderIoFreeCount++] = io;
    }

    return status;
}

NTSTATUS
CViosndMiniportWaveRTStream::SubmitRenderSilencePacket(
    _In_ ULONG PacketNumber)
{
    NTSTATUS status;
    PVIOSND_PCM_IO io;
    ULONG silenceLength;

    if (m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (m_RenderIoFreeCount == 0) {
        return STATUS_DEVICE_BUSY;
    }
    silenceLength = m_RenderFallbackActive ? VIOSND_FALLBACK_PERIOD_BYTES : m_PacketSize;
    if (silenceLength > sizeof(ViosndSilencePeriod)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    io = m_RenderIoPool[--m_RenderIoFreeCount];
    m_RenderIoPool[m_RenderIoFreeCount] = NULL;

    status = ViosndSubmitPreparedWritePcm(m_Device,
                                          m_StreamId,
                                          ViosndSilencePeriod,
                                          silenceLength,
                                          silenceLength,
                                          PacketNumber,
                                          io);
    if (NT_SUCCESS(status)) {
        if (PacketNumber != MAXULONG) {
            m_NextSubmitPacket = PacketNumber + 1;
        }
        m_OutstandingWrites++;
        if ((PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render preroll silence packet=%u outstanding=%u remaining=%u\n",
                       m_StreamId,
                       PacketNumber,
                       m_OutstandingWrites,
                       m_RenderPrerollPackets);
        }
    } else {
        m_RenderIoPool[m_RenderIoFreeCount++] = io;
    }
    return status;
}

VOID
CViosndMiniportWaveRTStream::ReclaimRenderPackets(_Inout_ PULONG SubmittedSinceLog)
{
    for (;;) {
        ULONG bytesWritten = 0;
        ULONG latencyBytes = 0;
        PVIOSND_PCM_IO io = NULL;
        NTSTATUS status = ViosndReclaimPreparedWritePcm(m_Device,
                                                        &io,
                                                        &bytesWritten,
                                                        &latencyBytes);

        if (status == STATUS_NOT_FOUND) {
            return;
        }

        if (io == NULL) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u reclaim render returned 0x%08x without io outstanding=%u next=%u\n",
                       m_StreamId,
                       status,
                       m_OutstandingWrites,
                       m_NextSubmitPacket);
            return;
        }

        if (m_OutstandingWrites != 0) {
            m_OutstandingWrites--;
        }

        if (NT_SUCCESS(status)) {
            ULONG packetNumber = ViosndGetPcmIoPacketNumber(io);

            if (packetNumber == MAXULONG) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u render reclaim preroll bytes=%u pos=%llu latency=%u outstanding=%u next=%u last=%u\n",
                           m_StreamId,
                           bytesWritten,
                           m_Position,
                           latencyBytes,
                           m_OutstandingWrites,
                           m_NextSubmitPacket,
                           m_LastOsWritePacket);
            } else {
                ULONG sourceBytes = m_RenderFallbackActive ?
                                        ViosndGetPcmIoSourceLength(io) :
                                        bytesWritten;

                if (sourceBytes == 0) {
                    sourceBytes = bytesWritten;
                }
                m_Position += sourceBytes;
                m_LastCompletionQpc = KeQueryPerformanceCounter(NULL).QuadPart;
                m_PacketNumber++;
                (*SubmittedSinceLog)++;
                if ((m_PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0 ||
                    latencyBytes > m_PacketSize * 4) {
                    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                               DPFLTR_ERROR_LEVEL,
                               "viosnd: stream %u render reclaim packet=%u bytes=%u source=%u pos=%llu latency=%u outstanding=%u next=%u last=%u fallback=%u\n",
                               m_StreamId,
                               m_PacketNumber,
                               bytesWritten,
                               sourceBytes,
                               m_Position,
                               latencyBytes,
                               m_OutstandingWrites,
                               m_NextSubmitPacket,
                               m_LastOsWritePacket,
                               m_RenderFallbackActive);
                }
                if (m_NotificationEvent != NULL) {
                    KeSetEvent(m_NotificationEvent, IO_NO_INCREMENT, FALSE);
                }
            }
        } else {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u reclaim render packet failed 0x%08x outstanding=%u\n",
                       m_StreamId,
                       status,
                       m_OutstandingWrites);
        }

        if (io != NULL && m_RenderIoFreeCount < VIOSND_RENDER_IO_POOL_SIZE) {
            m_RenderIoPool[m_RenderIoFreeCount++] = io;
        } else {
        ViosndFreeWritePcmIo(m_Device, io);
        }
    }
}

VOID
CViosndMiniportWaveRTStream::CancelRenderWrites(_In_ NTSTATUS FailureStatus)
{
    ULONG reclaimed = 0;
    ULONG detached = 0;

    if (m_Capture) {
        return;
    }

    ReclaimRenderPackets(&reclaimed);

    while (m_OutstandingWrites != 0) {
        PVIOSND_PCM_IO io = NULL;
        NTSTATUS status = ViosndDetachUnusedWritePcm(m_Device, &io);

        if (!NT_SUCCESS(status)) {
            break;
        }

        if (m_OutstandingWrites != 0) {
            m_OutstandingWrites--;
        }
        detached++;

        if (io != NULL && m_RenderIoFreeCount < VIOSND_RENDER_IO_POOL_SIZE) {
            m_RenderIoPool[m_RenderIoFreeCount++] = io;
        } else {
            ViosndFreeWritePcmIo(m_Device, io);
        }
    }

    if (reclaimed != 0 || detached != 0 || m_OutstandingWrites != 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render startup cleanup failure=0x%08x reclaimed=%u detached=%u outstanding=%u free=%u\n",
                   m_StreamId,
                   FailureStatus,
                   reclaimed,
                   detached,
                   m_OutstandingWrites,
                   m_RenderIoFreeCount);
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::AllocateRenderIoPool()
{
    if (m_Capture || m_RenderIoFreeCount != 0) {
        return STATUS_SUCCESS;
    }

    if (m_PacketSize == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (m_RenderFallbackBuffer == NULL) {
        m_RenderFallbackBufferSize = max(VIOSND_FALLBACK_PERIOD_BYTES, m_PacketSize);
        m_RenderFallbackBuffer = ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                             m_RenderFallbackBufferSize,
                                                             VIOSND_POOL_TAG);
        if (m_RenderFallbackBuffer == NULL) {
            m_RenderFallbackBufferSize = 0;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    for (ULONG i = 0; i < VIOSND_RENDER_IO_POOL_SIZE; ++i) {
        PVIOSND_PCM_IO io = NULL;
        NTSTATUS status = ViosndAllocateWritePcmIo(m_Device,
                                                   max(m_PacketSize,
                                                       VIOSND_FALLBACK_PERIOD_BYTES),
                                                   &io);

        if (!NT_SUCCESS(status)) {
            FreeRenderIoPool();
            return status;
        }
        m_RenderIoPool[m_RenderIoFreeCount++] = io;
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u render preallocated tx buffers=%u packet=%u\n",
               m_StreamId,
               m_RenderIoFreeCount,
               m_PacketSize);
    return STATUS_SUCCESS;
}

VOID
CViosndMiniportWaveRTStream::FreeRenderIoPool()
{
    if (m_Capture) {
        return;
    }

    if (m_OutstandingWrites != 0) {
        return;
    }

    while (m_RenderIoFreeCount != 0) {
        PVIOSND_PCM_IO io = m_RenderIoPool[--m_RenderIoFreeCount];

        m_RenderIoPool[m_RenderIoFreeCount] = NULL;
            ViosndFreeWritePcmIo(m_Device, io);
    }

    if (m_RenderFallbackBuffer != NULL) {
        ExFreePoolWithTag(m_RenderFallbackBuffer, VIOSND_POOL_TAG);
        m_RenderFallbackBuffer = NULL;
        m_RenderFallbackBufferSize = 0;
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::AllocateCaptureIoPool()
{
    if (!m_Capture || m_CaptureIoFreeCount != 0) {
        return STATUS_SUCCESS;
    }

    if (m_PacketSize == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    for (ULONG i = 0; i < VIOSND_CAPTURE_IO_POOL_SIZE; ++i) {
        PVIOSND_PCM_IO io = NULL;
        NTSTATUS status = ViosndAllocateReadPcmIo(m_Device, m_PacketSize, &io);

        if (!NT_SUCCESS(status)) {
            FreeCaptureIoPool();
            return status;
        }
        m_CaptureIoPool[m_CaptureIoFreeCount++] = io;
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u capture preallocated rx buffers=%u packet=%u\n",
               m_StreamId,
               m_CaptureIoFreeCount,
               m_PacketSize);
    return STATUS_SUCCESS;
}

VOID
CViosndMiniportWaveRTStream::FreeCaptureIoPool()
{
    if (!m_Capture) {
        return;
    }

    if (m_CaptureInFlight != 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u capture io pool retained with in-flight=%u\n",
                   m_StreamId,
                   m_CaptureInFlight);
        return;
    }

    while (m_CaptureIoFreeCount != 0) {
        PVIOSND_PCM_IO io = m_CaptureIoPool[--m_CaptureIoFreeCount];

        m_CaptureIoPool[m_CaptureIoFreeCount] = NULL;
        ViosndFreeReadPcmIo(m_Device, io);
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::SubmitCapturePacket()
{
    PVIOSND_PCM_IO io;
    NTSTATUS status;

    if (!m_Capture ||
        m_Buffer == NULL ||
        m_PacketSize == 0 ||
        m_NotificationCount == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (m_CaptureIoFreeCount == 0) {
        return STATUS_DEVICE_BUSY;
    }

    if (m_CaptureInFlight >= m_NotificationCount) {
        return STATUS_DEVICE_BUSY;
    }

    io = m_CaptureIoPool[--m_CaptureIoFreeCount];
    m_CaptureIoPool[m_CaptureIoFreeCount] = NULL;

    status = ViosndSubmitPreparedReadPcm(m_Device,
                                         m_StreamId,
                                         m_PacketSize,
                                         m_NextSubmitPacket,
                                         io);
    if (NT_SUCCESS(status)) {
        m_NextSubmitPacket++;
        m_CaptureInFlight++;
    } else {
        m_CaptureIoPool[m_CaptureIoFreeCount++] = io;
    }

    return status;
}

VOID
CViosndMiniportWaveRTStream::ReclaimCapturePackets()
{
    for (;;) {
        PVIOSND_PCM_IO io = NULL;
        ULONG bytesRead = 0;
        ULONG latencyBytes = 0;
        NTSTATUS status = ViosndReclaimPreparedReadPcm(m_Device,
                                                       &io,
                                                       &bytesRead,
                                                       &latencyBytes);

        if (status == STATUS_NOT_FOUND) {
            return;
        }

        if (m_CaptureInFlight != 0) {
            m_CaptureInFlight--;
        }

        if (NT_SUCCESS(status) && io != NULL) {
            ULONG packetNumber = ViosndGetPcmIoPacketNumber(io);
            ULONG offset = (packetNumber % m_NotificationCount) * m_PacketSize;
            PVOID source = ViosndGetPcmIoAudioBuffer(io);
            PUCHAR destination = (PUCHAR)m_Buffer + offset;
            ULONG copyLength = min(bytesRead, m_PacketSize);
            BOOLEAN sixteenBit = (BOOLEAN)(m_WaveFormat.ContainerBits == 16 &&
                                           !m_WaveFormat.Float);
            ULONG inputPeak = sixteenBit ? ViosndPcmPeak16(source, copyLength) : 0;
            ULONG outputPeak;

#if VIOSND_CAPTURE_NOISE_GATE_PEAK != 0
            if (!sixteenBit) {
                /* Nothing here knows how to read this format's samples. Carrying them through
                 * untouched is right; measuring or gaining them as 16-bit would not be. */
                RtlCopyMemory(destination, source, copyLength);
                outputPeak = 0;
            } else if (inputPeak < VIOSND_CAPTURE_NOISE_GATE_PEAK) {
                RtlZeroMemory(destination, copyLength);
                outputPeak = 0;
            } else {
                outputPeak = ViosndPcmCopyGain16(destination,
                                                 source,
                                                 copyLength,
                                                 VIOSND_CAPTURE_SOFTWARE_GAIN);
            }
#else
            if (!sixteenBit) {
                RtlCopyMemory(destination, source, copyLength);
                outputPeak = 0;
            } else {
                outputPeak = ViosndPcmCopyGain16(destination,
                                                 source,
                                                 copyLength,
                                                 VIOSND_CAPTURE_SOFTWARE_GAIN);
            }
#endif

            if (inputPeak > m_CaptureMaxInputPeak) {
                m_CaptureMaxInputPeak = inputPeak;
            }
            if (outputPeak > m_CaptureMaxOutputPeak) {
                m_CaptureMaxOutputPeak = outputPeak;
            }
            m_CaptureBytesCopied += copyLength;

            if (bytesRead < m_PacketSize) {
                RtlZeroMemory(destination + bytesRead, m_PacketSize - bytesRead);
            }

            m_PacketNumber = packetNumber + 1;
            m_Position += m_PacketSize;
            m_OutstandingWrites = m_PacketNumber - m_NextReadPacket;
            if (m_OutstandingWrites > m_NotificationCount) {
                ULONG droppedPackets = m_OutstandingWrites - m_NotificationCount;

                m_NextReadPacket += droppedPackets;
                m_OutstandingWrites = m_NotificationCount;
                m_CaptureOverflows += droppedPackets;
            }

            if ((packetNumber & 0x7f) == 0 || bytesRead == 0 || latencyBytes > m_PacketSize * 4) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture complete packet=%u bytes=%u peak=%u/%u gain=%u gate=%u pos=%llu latency=%u inFlight=%u ready=%u nextRead=%u overflow=%u\n",
                           m_StreamId,
                           packetNumber,
                           bytesRead,
                           inputPeak,
                           outputPeak,
                           VIOSND_CAPTURE_SOFTWARE_GAIN,
                           VIOSND_CAPTURE_NOISE_GATE_PEAK,
                           m_Position,
                           latencyBytes,
                           m_CaptureInFlight,
                           m_OutstandingWrites,
                           m_NextReadPacket,
                           m_CaptureOverflows);
            }

            if (m_NotificationEvent != NULL) {
                KeSetEvent(m_NotificationEvent, IO_NO_INCREMENT, FALSE);
            }
        } else {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u reclaim capture packet failed 0x%08x inFlight=%u\n",
                       m_StreamId,
                       status,
                       m_CaptureInFlight);
        }

        if (io != NULL && m_CaptureIoFreeCount < VIOSND_CAPTURE_IO_POOL_SIZE) {
            m_CaptureIoPool[m_CaptureIoFreeCount++] = io;
        } else {
            ViosndFreeReadPcmIo(m_Device, io);
        }
    }
}

NTSTATUS
CViosndMiniportWaveRTStream::StartRenderWorker()
{
    HANDLE threadHandle = NULL;
    NTSTATUS status;

    if (m_WorkerStarted) {
        return STATUS_SUCCESS;
    }

    status = m_Capture ? AllocateCaptureIoPool() : AllocateRenderIoPool();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeClearEvent(&m_StopEvent);
    KeClearEvent(&m_KickEvent);
    AddRef();
    status = PsCreateSystemThread(&threadHandle,
                                  THREAD_ALL_ACCESS,
                                  NULL,
                                  NULL,
                                  NULL,
                                  ViosndRenderThread,
                                  this);
    if (!NT_SUCCESS(status)) {
        Release();
        return status;
    }

    status = ObReferenceObjectByHandle(threadHandle,
                                       SYNCHRONIZE,
                                       NULL,
                                       KernelMode,
                                       &m_ThreadObject,
                                       NULL);
    ZwClose(threadHandle);
    if (!NT_SUCCESS(status)) {
        KeSetEvent(&m_StopEvent, IO_NO_INCREMENT, FALSE);
        return status;
    }

    m_WorkerStarted = TRUE;
    return STATUS_SUCCESS;
}

BOOLEAN
CViosndMiniportWaveRTStream::StopRenderWorker()
{
    LARGE_INTEGER timeout;
    PVOID threadObject = m_ThreadObject;
    NTSTATUS waitStatus;

    if (threadObject == NULL) {
        m_WorkerStarted = FALSE;
        return TRUE;
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s stop worker begin outstanding=%u next=%u last=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               m_OutstandingWrites,
               m_NextSubmitPacket,
               m_LastOsWritePacket);
    KeSetEvent(&m_StopEvent, IO_NO_INCREMENT, FALSE);

    timeout.QuadPart = -(10LL * 1000LL *
                         (m_Capture ? VIOSND_CAPTURE_STOP_TIMEOUT_MS : VIOSND_RENDER_STOP_TIMEOUT_MS));
    waitStatus = KeWaitForSingleObject(threadObject,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       &timeout);
    if (waitStatus == STATUS_TIMEOUT) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u %s stop worker timeout outstanding=%u next=%u inFlight=%u\n",
                   m_StreamId,
                   m_Capture ? "capture" : "render",
                   m_OutstandingWrites,
                   m_NextSubmitPacket,
                   m_CaptureInFlight);
        if (m_Capture) {
            InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
            ObDereferenceObject(threadObject);
            m_ThreadObject = NULL;
            m_WorkerStarted = FALSE;
        }
        return FALSE;
    }

    ObDereferenceObject(threadObject);
    m_ThreadObject = NULL;
    m_WorkerStarted = FALSE;
    if (m_Capture) {
        FreeCaptureIoPool();
    } else {
        FreeRenderIoPool();
    }
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s stop worker done outstanding=%u next=%u last=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               m_OutstandingWrites,
               m_NextSubmitPacket,
               m_LastOsWritePacket);
    return TRUE;
}

/*
 * Where playback has reached, as reported to the audio engine.
 *
 * This has to agree with the position this driver reads the shared buffer from, because the
 * engine fills ahead of what it is told and this driver reads ahead of what it has sent: two
 * pointers moving around one cyclic buffer. Measuring it from the start of the run instead --
 * a free-running clock that nothing corrects -- let the two drift apart by 0.31s, at which point
 * the read pointer was passing through regions the engine had not written yet, and a steady tone
 * came out torn at eight period boundaries a second.
 *
 * So it is measured forward from the last period the endpoint actually took. The interpolation
 * only smooths the step between completions, which is what the engine wants from a position, and
 * it is clamped to one period so that a stalled endpoint cannot make the position run away from
 * what was really consumed.
 */
ULONGLONG
CViosndMiniportWaveRTStream::GetRenderClockPosition()
{
    LARGE_INTEGER qpc;
    ULONGLONG interpolated;

    if (m_Capture ||
        m_LastCompletionQpc == 0 ||
        m_QpcFrequency <= 0) {
        return m_Position;
    }

    qpc = KeQueryPerformanceCounter(NULL);
    interpolated = ViosndRenderBytesFromQpc(qpc.QuadPart - m_LastCompletionQpc,
                                            m_QpcFrequency,
                                            m_WaveFormat.SampleRate,
                                            ViosndFrameBytes(&m_WaveFormat));
    if (m_PacketSize != 0 && interpolated > m_PacketSize) {
        interpolated = m_PacketSize;
    }
    return m_Position + interpolated;
}

VOID
CViosndMiniportWaveRTStream::MaybeSwitchRenderFallback()
{
    LARGE_INTEGER qpc;
    LONGLONG elapsedQpc;
    ULONGLONG elapsedMs;
    ULONGLONG completedBytes;
    ULONGLONG expectedBytes;
    /* Same reason as the clock: how far behind the stream is can only be measured against its
     * own frame size. */
    const ULONGLONG bytesPerSecond =
        (ULONGLONG)m_WaveFormat.SampleRate * ViosndFrameBytes(&m_WaveFormat);

    /* The fallback is a 16kHz 16-bit stereo mode, and the downsampler that feeds it reads and
     * writes SHORTs. It has nothing to say about a stream of another shape, so it does not run
     * for one rather than producing something shaped wrongly. */
    if (m_WaveFormat.ContainerBits != 16 || m_WaveFormat.Channels != 2 || m_WaveFormat.Float) {
        return;
    }

    if (m_Capture ||
        m_RenderFallbackAttempted ||
        m_State != KSSTATE_RUN ||
        m_QpcFrequency <= 0) {
        return;
    }

    qpc = KeQueryPerformanceCounter(NULL);
    if (m_FallbackMonitorStartQpc == 0) {
        m_FallbackMonitorStartQpc = qpc.QuadPart;
        m_FallbackMonitorStartPosition = m_Position;
        return;
    }

    elapsedQpc = qpc.QuadPart - m_FallbackMonitorStartQpc;
    if (elapsedQpc <= 0) {
        return;
    }

    elapsedMs = ((ULONGLONG)elapsedQpc * 1000u) / (ULONGLONG)m_QpcFrequency;
    if (elapsedMs < VIOSND_FALLBACK_MONITOR_MS) {
        return;
    }

    completedBytes = m_Position - m_FallbackMonitorStartPosition;
    expectedBytes = ((ULONGLONG)elapsedQpc * bytesPerSecond) /
                    (ULONGLONG)m_QpcFrequency;
    if (expectedBytes == 0) {
        return;
    }

    if ((completedBytes * 100u) >=
        (expectedBytes * VIOSND_FALLBACK_TRIGGER_PERCENT)) {
        if (!m_RenderFallbackActive) {
            m_RenderFallbackAttempted = TRUE;
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render fallback not needed completed=%llu expected=%llu percent=%llu\n",
                       m_StreamId,
                       completedBytes,
                       expectedBytes,
                       (completedBytes * 100u) / expectedBytes);
        } else {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render fallback stable rate=%u completed=%llu expected=%llu percent=%llu\n",
                       m_StreamId,
                       VIOSND_FALLBACK_SAMPLE_RATE,
                       completedBytes,
                       expectedBytes,
                       (completedBytes * 100u) / expectedBytes);
            m_FallbackMonitorStartQpc = qpc.QuadPart;
            m_FallbackMonitorStartPosition = m_Position;
        }
        return;
    }

    (VOID)SwitchRenderToFallback();
}

NTSTATUS
CViosndMiniportWaveRTStream::SwitchRenderToFallback()
{
    NTSTATUS stopStatus;
    NTSTATUS releaseStatus;
    NTSTATUS status;
    LARGE_INTEGER frequency;
    LARGE_INTEGER qpc;

    m_RenderFallbackAttempted = TRUE;
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u render fallback begin rate=%u pos=%llu next=%u outstanding=%u packet=%u\n",
               m_StreamId,
               VIOSND_FALLBACK_SAMPLE_RATE,
               m_Position,
               m_NextSubmitPacket,
               m_OutstandingWrites,
               m_PacketNumber);

    stopStatus = ViosndStopPcm(m_Device, m_StreamId);
    releaseStatus = ViosndReleasePcm(m_Device, m_StreamId);
    CancelRenderWrites(!NT_SUCCESS(stopStatus) ? stopStatus : releaseStatus);
    if (!NT_SUCCESS(stopStatus) || !NT_SUCCESS(releaseStatus)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render fallback stop/release failed stop=0x%08x release=0x%08x outstanding=%u\n",
                   m_StreamId,
                   stopStatus,
                   releaseStatus,
                   m_OutstandingWrites);
        return !NT_SUCCESS(stopStatus) ? stopStatus : releaseStatus;
    }

    m_RenderFallbackActive = TRUE;
    m_RenderFallbackPhase = ViosndFallbackInitialPhase();
    status = ViosndConfigureFallbackPcm(m_Device, m_StreamId);
    if (NT_SUCCESS(status)) {
        ULONG prerollPackets = VIOSND_RENDER_START_PREROLL_PACKETS;

        while (prerollPackets != 0 &&
               m_OutstandingWrites < VIOSND_RENDER_START_PREROLL_PACKETS) {
            status = SubmitRenderSilencePacket(MAXULONG);
            if (!NT_SUCCESS(status)) {
                break;
            }
            prerollPackets--;
        }
    }
    if (NT_SUCCESS(status)) {
        status = ViosndStartPcm(m_Device, m_StreamId);
    }
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render fallback start failed 0x%08x\n",
                   m_StreamId,
                   status);
        m_RenderFallbackActive = FALSE;
        return status;
    }

    qpc = KeQueryPerformanceCounter(&frequency);
    m_RunStartPosition = m_Position;
    m_RunStartQpc = qpc.QuadPart;
    m_LastCompletionQpc = qpc.QuadPart;
    m_QpcFrequency = frequency.QuadPart;
    m_FallbackMonitorStartQpc = 0;
    m_FallbackMonitorStartPosition = 0;
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u render fallback active rate=%u pos=%llu next=%u outstanding=%u\n",
               m_StreamId,
               VIOSND_FALLBACK_SAMPLE_RATE,
               m_Position,
               m_NextSubmitPacket,
               m_OutstandingWrites);
    return STATUS_SUCCESS;
}

VOID
CViosndMiniportWaveRTStream::RenderWorkerLoop()
{
    LARGE_INTEGER interval;
    ULONG submittedSinceLog = 0;
    ULONG lastCompletedPackets = 0;
    ULONG targetOutstanding;
    PVOID waitObjects[2];

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    interval.QuadPart = -(10LL * VIOSND_RENDER_POLL_INTERVAL_US);
    waitObjects[0] = &m_StopEvent;
    waitObjects[1] = &m_KickEvent;

    if (m_Capture) {
        CaptureWorkerLoop();
        return;
    }

    for (;;) {
        NTSTATUS waitStatus;

        waitStatus = KeWaitForMultipleObjects(SIZEOF_ARRAY(waitObjects),
                                              waitObjects,
                                              WaitAny,
                                              Executive,
                                              KernelMode,
                                              FALSE,
                                              &interval,
                                              NULL);
        if (waitStatus == STATUS_WAIT_0) {
            break;
        }

        ViosndPollEvents(m_Device);
        ReclaimRenderPackets(&submittedSinceLog);
        MaybeSwitchRenderFallback();
        if (m_PacketNumber != lastCompletedPackets) {
            lastCompletedPackets = m_PacketNumber;
            m_RenderStallLoops = 0;
        } else if (m_State == KSSTATE_RUN && m_OutstandingWrites != 0) {
            m_RenderStallLoops++;
            if ((m_RenderStallLoops % VIOSND_RENDER_REKICK_STALL_LOOPS) == 0) {
                ViosndKickTxQueue(m_Device);
            }
            if ((m_RenderStallLoops % VIOSND_COMPLETION_STALL_LOG_LOOPS) == 0) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u render no completion loops=%u outstanding=%u next=%u last=%u pos=%llu rekick=%u\n",
                           m_StreamId,
                           m_RenderStallLoops,
                           m_OutstandingWrites,
                           m_NextSubmitPacket,
                           m_LastOsWritePacket,
                           m_Position,
                           m_RenderStallLoops / 50u);
            }
        }

        if (m_State != KSSTATE_RUN) {
            continue;
        }

        if (m_Buffer == NULL ||
            m_PacketSize == 0) {
            continue;
        }

        if (m_RenderFallbackActive) {
            targetOutstanding = ViosndFallbackTargetOutstandingPackets(m_NotificationCount);
        } else {
            targetOutstanding = m_LastOsWritePacket == MAXULONG ?
                                    ViosndCyclicTargetOutstandingPackets(m_NotificationCount) :
                                    ViosndTargetOutstandingPackets(m_NotificationCount);
        }
        /* The host may have an opinion, set by the user as a latency choice rather than guessed
         * by the driver. It can only raise the floor, never exceed NotificationCount - 1: the
         * pump must not send a packet the OS has not written. Zero means no opinion. */
        targetOutstanding = ViosndApplyHostOutstandingHint(m_Device,
                                                           targetOutstanding,
                                                           m_NotificationCount);
        while (m_State == KSSTATE_RUN &&
               m_OutstandingWrites < targetOutstanding) {
            ULONG packetLength = m_PacketSize;
            NTSTATUS status;

            if (ViosndHasWritableRenderPacket(m_NextSubmitPacket, m_LastOsWritePacket)) {
                if (m_EosPacketNumber == m_NextSubmitPacket) {
                    packetLength = m_EosPacketLength;
                }

                status = SubmitRenderPacket(m_NextSubmitPacket, packetLength);
            } else if (m_LastOsWritePacket == MAXULONG) {
                if (m_RenderPrerollPackets != 0) {
                    status = SubmitRenderSilencePacket(m_NextSubmitPacket);
                    if (NT_SUCCESS(status)) {
                        m_RenderPrerollPackets--;
                    }
                } else {
                    status = SubmitRenderPacket(m_NextSubmitPacket, m_PacketSize);
                }
                if (NT_SUCCESS(status)) {
                    m_RenderFallbackPackets++;
                    if ((m_RenderFallbackPackets & VIOSND_PERIODIC_LOG_MASK) == 1) {
                        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                                   DPFLTR_ERROR_LEVEL,
                                   "viosnd: stream %u render cyclic packet=%u outstanding=%u total=%u\n",
                                   m_StreamId,
                                   m_NextSubmitPacket - 1,
                                   m_OutstandingWrites,
                                   m_RenderFallbackPackets);
                    }
                }
            } else {
                if (m_OutstandingWrites == 0) {
                    m_RenderNotReadyLoops++;
                    if ((m_RenderNotReadyLoops & 0x7f) == 1) {
                        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                                   DPFLTR_ERROR_LEVEL,
                                   "viosnd: stream %u render packet not ready next=%u last=%u loops=%u\n",
                                   m_StreamId,
                                   m_NextSubmitPacket,
                                   m_LastOsWritePacket,
                                   m_RenderNotReadyLoops);
                    }
                }
                break;
            }

            if (!NT_SUCCESS(status)) {
                break;
            }
        }
    }

    if (m_OutstandingWrites != 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render worker exit with pending tx outstanding=%u next=%u last=%u pos=%llu\n",
                   m_StreamId,
                   m_OutstandingWrites,
                   m_NextSubmitPacket,
                   m_LastOsWritePacket,
                   m_Position);
    }
}

VOID
CViosndMiniportWaveRTStream::CaptureWorkerLoop()
{
    LARGE_INTEGER interval;
    ULONG lastCompletedPackets = 0;
    ULONG targetOutstanding;
    ULONG diagTicks = 0;
    BOOLEAN skipRecorded = FALSE;
    BOOLEAN failureRecorded = FALSE;

    interval.QuadPart = -(10LL * VIOSND_CAPTURE_POLL_INTERVAL_US);

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u capture worker enter packet=%u size=%u notif=%u\n",
               m_StreamId,
               m_NextSubmitPacket,
               m_PacketSize,
               m_NotificationCount);

    /*
     * The host's in-flight hint applies here too. It was only ever read on the render side, so
     * choosing a buffer depth in the UI did nothing at all to a microphone -- the capture pump
     * kept its own constant, and the setting was a control that moved nothing.
     *
     * The failure it governs is the mirror image: playback starves when the guest is late with a
     * period, capture drops audio when the guest has not left a buffer for the host to fill. The
     * host reports the second as an overrun, and the depth is what decides how much slack there
     * is before that happens.
     */
    targetOutstanding = ViosndApplyHostOutstandingHint(m_Device,
                                                       VIOSND_CAPTURE_TARGET_OUTSTANDING_PACKETS,
                                                       m_NotificationCount);
    targetOutstanding = min(targetOutstanding,
                            min(m_NotificationCount, VIOSND_CAPTURE_IO_POOL_SIZE));
    targetOutstanding = max(targetOutstanding, 1u);

    /*
     * Every one of these can stop the loop below from ever submitting, and each does it by
     * taking a branch that says nothing: SubmitCapturePacket returns
     * STATUS_INVALID_DEVICE_REQUEST when the buffer geometry is unset, and the loop swallows
     * STATUS_DEVICE_BUSY. From outside, all of that looks the same as a device that simply
     * never records. Write down what it started with.
     */
    {
        WCHAR text[160];

        if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                           SIZEOF_ARRAY(text),
                                           L"enter: notif=%u packet=%u buffer=%u pool=%u "
                                           L"target=%u state=%u",
                                           m_NotificationCount,
                                           m_PacketSize,
                                           m_BufferSize,
                                           m_CaptureIoFreeCount,
                                           targetOutstanding,
                                           m_State))) {
            ViosndRecordDiag(m_Device, L"CaptureWorker", text);
        }
    }

    while (m_State == KSSTATE_RUN) {
        NTSTATUS waitStatus;

        waitStatus = KeWaitForSingleObject(&m_StopEvent,
                                           Executive,
                                           KernelMode,
                                           FALSE,
                                           &interval);
        if (waitStatus == STATUS_SUCCESS) {
            break;
        }

        ReclaimCapturePackets();
        if (m_PacketNumber != lastCompletedPackets) {
            lastCompletedPackets = m_PacketNumber;
            m_CaptureStallLoops = 0;
        } else if (m_State == KSSTATE_RUN && m_CaptureInFlight != 0) {
            m_CaptureStallLoops++;
            if ((m_CaptureStallLoops % VIOSND_COMPLETION_STALL_LOG_LOOPS) == 0) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture no completion loops=%u inFlight=%u next=%u pos=%llu\n",
                           m_StreamId,
                           m_CaptureStallLoops,
                           m_CaptureInFlight,
                           m_NextSubmitPacket,
                           m_Position);
            }
        }

        if (m_State != KSSTATE_RUN ||
            m_Buffer == NULL ||
            m_PacketSize == 0 ||
            m_NotificationCount == 0) {
            /* Say which of the four it is, once. Skipping the submit for any of these reasons
             * is indistinguishable from the outside from a device that never records. */
            if (!skipRecorded) {
                WCHAR text[160];

                skipRecorded = TRUE;
                if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                                   SIZEOF_ARRAY(text),
                                                   L"skipping: state=%u buffer=%p packet=%u "
                                                   L"notif=%u loop=%u",
                                                   m_State,
                                                   m_Buffer,
                                                   m_PacketSize,
                                                   m_NotificationCount,
                                                   diagTicks))) {
                    ViosndRecordDiag(m_Device, L"CaptureWorker", text);
                }
            }
            diagTicks++;
            continue;
        }

        while (m_State == KSSTATE_RUN && m_CaptureInFlight < targetOutstanding) {
            NTSTATUS status = SubmitCapturePacket();

            if (NT_SUCCESS(status)) {
                m_CaptureSubmitOk++;
                continue;
            }
            m_CaptureSubmitFail++;
            m_CaptureLastSubmitStatus = status;
            if (status != STATUS_DEVICE_BUSY) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture submit failed 0x%08x next=%u inFlight=%u\n",
                           m_StreamId,
                           status,
                           m_NextSubmitPacket,
                           m_CaptureInFlight);
            }
            break;
        }

        /*
         * This line used to go out once a second for as long as the microphone was open. The
         * enter/exit pair now brackets the worker and the exit line carries the same counters,
         * so the heartbeat only ever said early what the end would say -- except in the one case
         * the exit line cannot cover, which is a worker that never reaches its exit. What is
         * worth keeping out of it is therefore the first failed submission, written once.
         */
        ++diagTicks;
        if (m_CaptureSubmitFail != 0 && !failureRecorded) {
            WCHAR text[192];

            failureRecorded = TRUE;
            if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                               SIZEOF_ARRAY(text),
                                               L"first submit failure: ok=%u fail=%u last=0x%08x "
                                               L"inFlight=%u next=%u read=%u free=%u ready=%u "
                                               L"state=%u loop=%u",
                                               m_CaptureSubmitOk,
                                               m_CaptureSubmitFail,
                                               m_CaptureLastSubmitStatus,
                                               m_CaptureInFlight,
                                               m_NextSubmitPacket,
                                               m_CaptureReadPackets,
                                               m_CaptureIoFreeCount,
                                               m_OutstandingWrites,
                                               m_State,
                                               diagTicks))) {
                ViosndRecordDiag(m_Device, L"CaptureWorker", text);
            }
        }
    }

    /* The loop is gone; say why, and what it managed. Its absence used to be visible only as a
     * host complaining that its buffers went nowhere. */
    {
        WCHAR text[224];

        if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                           SIZEOF_ARRAY(text),
                                           L"exit: state=%u loops=%u ok=%u fail=%u "
                                           L"inFlight=%u next=%u read=%u posq=%u pos=%llu "
                                           L"done=%u inPeak=%u outPeak=%u copied=%u",
                                           m_State,
                                           diagTicks,
                                           m_CaptureSubmitOk,
                                           m_CaptureSubmitFail,
                                           m_CaptureInFlight,
                                           m_NextSubmitPacket,
                                           m_CaptureReadPackets,
                                           m_CapturePositionQueries,
                                           m_Position,
                                           m_PacketNumber,
                                           m_CaptureMaxInputPeak,
                                           m_CaptureMaxOutputPeak,
                                           m_CaptureBytesCopied))) {
            ViosndRecordDiag(m_Device, L"CaptureWorker", text);
        }
    }

    for (ULONG i = 0; i < VIOSND_CAPTURE_STOP_RECLAIM_POLLS && m_CaptureInFlight != 0; ++i) {
        ReclaimCapturePackets();
        if (m_CaptureInFlight == 0) {
            break;
        }
    }

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u capture worker exit packet=%u position=%llu state=%u inFlight=%u\n",
               m_StreamId,
               m_NextSubmitPacket,
               m_Position,
               m_State,
               m_CaptureInFlight);
}

static VOID
ViosndRenderThread(_In_ PVOID Context)
{
    CViosndMiniportWaveRTStream *stream = (CViosndMiniportWaveRTStream *)Context;

    stream->RenderWorkerLoop();
    stream->Release();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    NTSTATUS status = STATUS_SUCCESS;
    KSSTATE oldState = m_State;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s SetState begin old=%u new=%u worker=%u outstanding=%u packet=%u next=%u last=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               oldState,
               State,
               m_WorkerStarted,
               m_OutstandingWrites,
               m_PacketNumber,
               m_NextSubmitPacket,
               m_LastOsWritePacket);

    if (State == KSSTATE_RUN && oldState != KSSTATE_RUN) {
        if (!m_Capture) {
            LARGE_INTEGER frequency;
            LARGE_INTEGER qpc;

            m_RenderUnderruns = 0;
            m_RenderFallbackPackets = 0;
            m_RenderPrerollPackets = oldState == KSSTATE_STOP ?
                                         VIOSND_RENDER_CYCLIC_PREROLL_PACKETS :
                                         0;
            m_RenderStallLoops = 0;
            m_RenderNotReadyLoops = 0;
            m_RenderPositionQueries = 0;
            m_RenderFallbackPhase = 0;
            m_RenderFallbackActive = FALSE;
            m_RenderFallbackAttempted = FALSE;
            m_FallbackMonitorStartPosition = 0;
            m_FallbackMonitorStartQpc = 0;
            m_RunStartPosition = 0;
            m_RunStartQpc = 0;
            m_LastCompletionQpc = 0;
            m_QpcFrequency = 0;
            status = ConfigureNegotiatedPcm();
            if (NT_SUCCESS(status)) {
                status = AllocateRenderIoPool();
            }
            if (NT_SUCCESS(status)) {
                ULONG prerollPackets = VIOSND_RENDER_START_PREROLL_PACKETS;

                while (prerollPackets != 0 &&
                       m_OutstandingWrites < VIOSND_RENDER_START_PREROLL_PACKETS) {
                    status = SubmitRenderSilencePacket(MAXULONG);
                    if (!NT_SUCCESS(status)) {
                        break;
                    }
                    prerollPackets--;
                }
                if (NT_SUCCESS(status)) {
                    m_RenderPrerollPackets = 0;
                    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                               DPFLTR_ERROR_LEVEL,
                               "viosnd: stream %u render prequeued silence outstanding=%u next=%u\n",
                               m_StreamId,
                               m_OutstandingWrites,
                               m_NextSubmitPacket);
                }
            }
            if (NT_SUCCESS(status)) {
                status = ViosndStartPcm(m_Device, m_StreamId);
            }
            if (NT_SUCCESS(status)) {
                qpc = KeQueryPerformanceCounter(&frequency);
                m_RunStartPosition = m_Position;
                m_RunStartQpc = qpc.QuadPart;
                m_LastCompletionQpc = qpc.QuadPart;
                m_QpcFrequency = frequency.QuadPart;
                m_State = State;
                status = StartRenderWorker();
                if (!NT_SUCCESS(status)) {
                    ViosndStopPcm(m_Device, m_StreamId);
                    ViosndReleasePcm(m_Device, m_StreamId);
                    m_State = oldState;
                }
            }
            if (!NT_SUCCESS(status)) {
                CancelRenderWrites(status);
                FreeRenderIoPool();
                m_RunStartPosition = 0;
                m_RunStartQpc = 0;
                m_QpcFrequency = 0;
                m_State = oldState;
            }

            /*
             * The geometry the engine settled on, recorded where recording it works.
             *
             * This used to be taken from GetPosition, which is where the numbers are most
             * obviously to hand -- and it never appeared, because that path runs above
             * PASSIVE_LEVEL and the writer refuses there. The old code only ever got a value out
             * by trying again every 256 queries until one landed. A state transition is the
             * right place for it anyway: portcls calls SetState at PASSIVE_LEVEL, it happens
             * once, and it is where the geometry becomes true. The capture side has said the
             * same thing from the same place all along.
             */
            {
                WCHAR text[192];

                if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                                   SIZEOF_ARRAY(text),
                                                   L"status=0x%08x packet=%u notif=%u buffer=%u "
                                                   L"outstanding=%u next=%u preroll=%u",
                                                   status,
                                                   m_PacketSize,
                                                   m_NotificationCount,
                                                   m_BufferSize,
                                                   m_OutstandingWrites,
                                                   m_NextSubmitPacket,
                                                   m_RenderPrerollPackets))) {
                    ViosndRecordDiag(m_Device, L"RenderStart", text);
                }
            }
        } else {
            /*
             * ViosndCaptureFaulted is no longer a reason to refuse.
             *
             * It is a process-wide latch, set when a capture stream failed to stop or release,
             * and once set it turned every later attempt to open capture -- on any stream, on
             * any card -- into STATUS_DEVICE_NOT_READY until the driver was reloaded. That made
             * one transient failure permanent and global, which is a worse outcome than the
             * failure it was guarding against. The recovery it was standing in for is now done
             * directly below: the stream is stopped and released before it is configured, so a
             * device left in a bad state by the previous stream is put right rather than
             * remembered. The latch stays as a record of the fault having happened.
             */
            m_PacketNumber = 0;
            m_NextSubmitPacket = 0;
            m_NextReadPacket = 0;
            m_OutstandingWrites = 0;
            m_CaptureInFlight = 0;
            m_CaptureOverflows = 0;
            m_CaptureStallLoops = 0;
            m_CapturePositionQueries = 0;
            m_CaptureReadPackets = 0;
            m_Position = 0;
            NTSTATUS configureStatus;
            NTSTATUS startStatus = STATUS_UNSUCCESSFUL;
            NTSTATUS workerStatus = STATUS_UNSUCCESSFUL;

            /*
             * Put the device back to a state SET_PARAMS is legal from, whatever the last stream
             * left behind.
             *
             * virtio-snd's stream states run SET_PARAMS -> PREPARE -> START -> STOP -> RELEASE,
             * and SET_PARAMS from STARTED is refused. A previous stream that failed to stop --
             * which this driver's own stop path gives up on after a timeout -- leaves the device
             * STARTED, and then every later attempt to open the stream is rejected:
             *
             *   Invalid PCM state transition from VIRTIO_SND_R_PCM_START to
             *   VIRTIO_SND_R_PCM_SET_PARAMS
             *
             * The endpoint stays present and returns silence, permanently, until the VM is
             * restarted. Both calls are expected to fail when the stream was already idle, which
             * is why neither status is examined: this is about the state the device is in, not
             * about whether these two particular messages succeeded.
             */
            (VOID)ViosndStopPcm(m_Device, m_StreamId);
            (VOID)ViosndReleasePcm(m_Device, m_StreamId);

            configureStatus = ConfigureNegotiatedPcm();
            status = configureStatus;
            if (NT_SUCCESS(status)) {
                startStatus = ViosndStartPcm(m_Device, m_StreamId);
                status = startStatus;
            }
            if (NT_SUCCESS(status)) {
                m_State = State;
                workerStatus = StartRenderWorker();
                status = workerStatus;
                if (!NT_SUCCESS(status)) {
                    ViosndStopPcm(m_Device, m_StreamId);
                    m_State = oldState;
                }
            } else {
                m_State = oldState;
            }

            /* Each of these three can leave the worker unstarted, and each said so only to a
             * debugger. From the host all three look the same: buffers offered, none taken. */
            {
                WCHAR text[160];

                if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                                   SIZEOF_ARRAY(text),
                                                   L"configure=0x%08x start=0x%08x worker=0x%08x "
                                                   L"packet=%u notif=%u buffer=%u",
                                                   configureStatus,
                                                   startStatus,
                                                   workerStatus,
                                                   m_PacketSize,
                                                   m_NotificationCount,
                                                   m_BufferSize))) {
                    ViosndRecordDiag(m_Device, L"CaptureStart", text);
                }
            }
        }
    } else if (State != KSSTATE_RUN && oldState == KSSTATE_RUN) {
        m_State = State;
        if (m_Capture) {
            NTSTATUS releaseStatus;
            NTSTATUS prepareStatus = STATUS_DEVICE_NOT_READY;
            ULONG detached = 0;
            BOOLEAN workerStopped = TRUE;

            if (m_WorkerStarted) {
                workerStopped = StopRenderWorker();
            }

            /*
             * STOP, then RELEASE. Both, in that order.
             *
             * This used to send RELEASE alone, on the reasoning that RELEASE is what flushes
             * pending I/O and QEMU's input STOP could block while RX buffers were still queued.
             * The spec does not allow it: RELEASE is only legal from PREPARE or STOP, and a
             * device that enforces that -- crosvm does -- refuses it outright:
             *
             *   Invalid PCM state transition from VIRTIO_SND_R_PCM_START to
             *   VIRTIO_SND_R_PCM_RELEASE
             *
             * The refusal was then treated as a fault, latched process-wide, and the stream was
             * left STARTED, so the next attempt to open capture had SET_PARAMS refused for the
             * same reason. That is the whole of why capture worked at most once.
             */
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u capture stop/release begin workerStopped=%u inFlight=%u\n",
                       m_StreamId,
                       workerStopped,
                       m_CaptureInFlight);
            (VOID)ViosndStopPcm(m_Device, m_StreamId);
            releaseStatus = ViosndReleasePcm(m_Device, m_StreamId);
            if (NT_SUCCESS(releaseStatus)) {
                ReclaimCapturePackets();
                detached = ViosndDetachUnusedReadPcm(m_Device);
                m_CaptureInFlight = 0;
                m_OutstandingWrites = 0;
                FreeCaptureIoPool();
                /*
                 * Deliberately not configuring the stream again here.
                 *
                 * This used to re-run SET_PARAMS and PREPARE straight after the release, to have
                 * the stream ready for the next RUN. PREPARE is what makes the host open its
                 * microphone, so the effect was that closing a recording application released
                 * the stream and reopened it fourteen milliseconds later -- and left it open.
                 * On Android that means the recording indicator stays lit for as long as the VM
                 * runs, once anything in it has ever recorded, which is a privacy signal saying
                 * something untrue.
                 *
                 * Nothing is lost: the start path stops and releases the stream before
                 * configuring it, so it does not depend on having been prepared in advance.
                 */
                prepareStatus = STATUS_SUCCESS;
            } else {
                InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
            }
            if (!workerStopped) {
                InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
            }
            if (workerStopped &&
                NT_SUCCESS(releaseStatus) &&
                !NT_SUCCESS(prepareStatus)) {
                InterlockedExchange((volatile LONG *)&ViosndCaptureFaulted, 1);
            }
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u capture release/drain release=0x%08x detached=%u prepare=0x%08x workerStopped=%u faulted=%ld\n",
                       m_StreamId,
                       releaseStatus,
                       detached,
                       prepareStatus,
                       workerStopped,
                       InterlockedCompareExchange((volatile LONG *)&ViosndCaptureFaulted, 0, 0));
            {
                WCHAR text[160];

                if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                                   SIZEOF_ARRAY(text),
                                                   L"release=0x%08x prepare=0x%08x detached=%u "
                                                   L"workerStopped=%u",
                                                   releaseStatus,
                                                   prepareStatus,
                                                   detached,
                                                   workerStopped))) {
                    ViosndRecordDiag(m_Device, L"CaptureStop", text);
                }
            }
            status = STATUS_SUCCESS;
        } else {
            NTSTATUS stopStatus;
            NTSTATUS releaseStatus;
            BOOLEAN workerStopped = TRUE;

            if (m_WorkerStarted) {
                workerStopped = StopRenderWorker();
            }
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render stop pcm begin\n",
                       m_StreamId);
            stopStatus = ViosndStopPcm(m_Device, m_StreamId);
            releaseStatus = ViosndReleasePcm(m_Device, m_StreamId);
            if (!workerStopped && m_WorkerStarted) {
                workerStopped = StopRenderWorker();
            }
            CancelRenderWrites(!NT_SUCCESS(stopStatus) ? stopStatus : releaseStatus);
            FreeRenderIoPool();
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u render stop/release done stop=0x%08x release=0x%08x workerStopped=%u outstanding=%u free=%u\n",
                       m_StreamId,
                       stopStatus,
                       releaseStatus,
                       workerStopped,
                       m_OutstandingWrites,
                       m_RenderIoFreeCount);
            status = workerStopped ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
            if (NT_SUCCESS(status)) {
                m_Position = GetRenderClockPosition();
                m_RunStartPosition = 0;
                m_RunStartQpc = 0;
                m_QpcFrequency = 0;
            }
        }
        if (!NT_SUCCESS(status)) {
            m_State = oldState;
        } else if (State == KSSTATE_STOP) {
            m_PacketNumber = 0;
            m_LastOsWritePacket = MAXULONG;
            m_NextSubmitPacket = 0;
            m_NextReadPacket = 0;
            m_EosPacketNumber = MAXULONG;
            m_EosPacketLength = 0;
            m_OutstandingWrites = 0;
            m_RenderUnderruns = 0;
            m_RenderFallbackPackets = 0;
            m_RenderPrerollPackets = 0;
            m_RenderStallLoops = 0;
            m_CaptureStallLoops = 0;
            m_RenderPositionQueries = 0;
            m_RenderNotReadyLoops = 0;
            m_RenderFallbackPhase = 0;
            m_RenderFallbackActive = FALSE;
            m_RenderFallbackAttempted = FALSE;
            m_FallbackMonitorStartPosition = 0;
            m_FallbackMonitorStartQpc = 0;
            m_RunStartPosition = 0;
            m_RunStartQpc = 0;
            m_QpcFrequency = 0;
            if (m_Capture && m_CaptureInFlight != 0) {
                VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                           DPFLTR_ERROR_LEVEL,
                           "viosnd: stream %u capture stopped with in-flight=%u\n",
                           m_StreamId,
                           m_CaptureInFlight);
            } else {
                m_CaptureInFlight = 0;
            }
            m_Position = 0;
        }
    } else {
        m_State = State;
    }

    if (NT_SUCCESS(status)) {
        if (State == KSSTATE_RUN && !m_Capture) {
            KeSetEvent(&m_KickEvent, IO_NO_INCREMENT, FALSE);
        }
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u %s state=%u lastWrite=%u nextSubmit=%u outstanding=%u worker=%u buf=%u packet=%u notif=%u\n",
                   m_StreamId,
                   m_Capture ? "capture" : "render",
                   State,
                   m_LastOsWritePacket,
                   m_NextSubmitPacket,
                   m_OutstandingWrites,
                   m_WorkerStarted,
                   m_BufferSize,
                   m_PacketSize,
                   m_NotificationCount);
    } else {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u %s SetState(%u) failed 0x%08x\n",
                   m_StreamId,
                   m_Capture ? "capture" : "render",
                   State,
                   status);
    }
    return status;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    ULONGLONG currentPosition;
    ULONGLONG playOffset;
    ULONGLONG writeLead;
    ULONG targetOutstanding;

    if (m_BufferSize == 0) {
        Position->PlayOffset = 0;
        Position->WriteOffset = 0;
        return STATUS_SUCCESS;
    }

    if (m_Capture) {
        ULONGLONG recordOffset;

        /*
         * For a capture stream these two are not the same thing. PlayOffset is how far the
         * client may safely read; WriteOffset is where the device is writing now. The OS takes
         * the distance between them as the amount of new audio available, so reporting one
         * value for both says "nothing to read" -- every time, however much was actually
         * recorded.
         *
         * That is what happened: the driver posted buffers, the device filled them, the
         * position advanced at exactly the right rate, and the OS polled about ninety times,
         * saw nothing available on any of them, and stopped the stream after less than half a
         * second. The recording came back silent with nothing anywhere reporting an error.
         *
         * The device has finished writing everything up to m_Position, so that is the read
         * limit; it is filling the packet after it.
         */
        recordOffset = m_Position % m_BufferSize;
        Position->PlayOffset = recordOffset;
        Position->WriteOffset = (m_Position + m_PacketSize) % m_BufferSize;
        m_CapturePositionQueries++;
        if (m_CapturePositionQueries <= 8 ||
            (m_CapturePositionQueries & 0x7f) == 0) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: stream %u capture GetPosition count=%u posOffset=%llu pos=%llu inFlight=%u nextSubmit=%u\n",
                       m_StreamId,
                       m_CapturePositionQueries,
                       recordOffset,
                       m_Position,
                       m_CaptureInFlight,
                       m_NextSubmitPacket);
        }
        return STATUS_SUCCESS;
    }

    currentPosition = GetRenderClockPosition();
    playOffset = currentPosition % m_BufferSize;
    if (m_RenderFallbackActive) {
        targetOutstanding = ViosndFallbackTargetOutstandingPackets(m_NotificationCount);
    } else {
        targetOutstanding = m_LastOsWritePacket == MAXULONG ?
                                ViosndCyclicTargetOutstandingPackets(m_NotificationCount) :
                                ViosndTargetOutstandingPackets(m_NotificationCount);
    }
    writeLead = (ULONGLONG)m_PacketSize * max(targetOutstanding, 1u);
    if (m_RenderFallbackActive) {
        writeLead *= 3u;
    }
    Position->PlayOffset = playOffset;
    Position->WriteOffset = (playOffset + writeLead) % m_BufferSize;
    m_RenderPositionQueries++;

    if (m_RenderPositionQueries <= 8 ||
        (m_RenderPositionQueries & 0x7f) == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render GetPosition count=%u play=%llu write=%llu qpcPos=%llu donePos=%llu next=%u outstanding=%u last=%u state=%u\n",
                   m_StreamId,
                   m_RenderPositionQueries,
                   Position->PlayOffset,
                   Position->WriteOffset,
                   currentPosition,
                   m_Position,
                   m_NextSubmitPacket,
                   m_OutstandingWrites,
                   m_LastOsWritePacket,
                   m_State);
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::AllocateAudioBuffer(
    _In_ ULONG RequestedSize,
    _Out_ PMDL *AudioBufferMdl,
    _Out_ ULONG *ActualSize,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS skip;
    ULONG allocationSize;

    low.QuadPart = 0;
    high.QuadPart = MAXLONGLONG;
    skip.QuadPart = 0;
    allocationSize = RequestedSize;
    allocationSize = max(allocationSize, VIOSND_DEFAULT_BUFFER_BYTES);

    m_Mdl = MmAllocatePagesForMdlEx(low, high, skip, allocationSize, MmCached, 0);
    if (m_Mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_Buffer = MmGetSystemAddressForMdlSafe(m_Mdl, NormalPagePriority | MdlMappingNoExecute);
    if (m_Buffer == NULL) {
        FreeAudioBuffer(m_Mdl, allocationSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG allocatedSize = MmGetMdlByteCount(m_Mdl);
    RtlZeroMemory(m_Buffer, allocatedSize);
    m_BufferSize = ViosndUsableBufferSizeFromAllocatedSize(allocatedSize, m_PeriodBytes);
    m_NotificationCount = ViosndNotificationCountFromBufferSize(m_BufferSize, m_PeriodBytes);
    m_PacketSize = ViosndPacketSizeFromNotificationCount(m_BufferSize,
                                                         m_NotificationCount,
                                                         m_PeriodBytes);
    *AudioBufferMdl = m_Mdl;
    *ActualSize = m_BufferSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: stream %u %s AllocateAudioBuffer requested=%u allocated=%u actual=%u packet=%u notif=%u\n",
               m_StreamId,
               m_Capture ? "capture" : "render",
               RequestedSize,
               allocatedSize,
               m_BufferSize,
               m_PacketSize,
               m_NotificationCount);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CViosndMiniportWaveRTStream::FreeAudioBuffer(_In_opt_ PMDL AudioBufferMdl, _In_ ULONG BufferSize)
{
    UNREFERENCED_PARAMETER(BufferSize);

    if (AudioBufferMdl != NULL) {
        MmFreePagesFromMdl(AudioBufferMdl);
        ExFreePool(AudioBufferMdl);
    }

    if (AudioBufferMdl == m_Mdl) {
        FreeRenderIoPool();
        FreeCaptureIoPool();
        m_Mdl = NULL;
        m_Buffer = NULL;
        m_BufferSize = 0;
        m_NotificationCount = 0;
        m_PacketSize = 0;
        m_PacketNumber = 0;
        m_LastOsWritePacket = MAXULONG;
        m_NextSubmitPacket = 0;
        m_NextReadPacket = 0;
        m_EosPacketNumber = MAXULONG;
        m_EosPacketLength = 0;
        m_OutstandingWrites = 0;
        m_RenderUnderruns = 0;
        m_RenderFallbackPackets = 0;
        m_RenderPrerollPackets = 0;
        m_RenderStallLoops = 0;
        m_CaptureStallLoops = 0;
        m_RenderPositionQueries = 0;
        m_RenderNotReadyLoops = 0;
        m_RenderFallbackPhase = 0;
        m_RenderFallbackActive = FALSE;
        m_RenderFallbackAttempted = FALSE;
        m_FallbackMonitorStartPosition = 0;
        m_FallbackMonitorStartQpc = 0;
        if (m_CaptureInFlight == 0) {
            m_CaptureOverflows = 0;
        }
        m_Position = 0;
    }
}

STDMETHODIMP_(VOID)
CViosndMiniportWaveRTStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY *Latency)
{
    RtlZeroMemory(Latency, sizeof(*Latency));
    Latency->FifoSize = VIOSND_DEFAULT_PERIOD_BYTES;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER *Register)
{
    RtlZeroMemory(Register, sizeof(*Register));
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER *Register)
{
    RtlZeroMemory(Register, sizeof(*Register));
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::AllocateBufferWithNotification(
    _In_ ULONG NotificationCount,
    _In_ ULONG RequestedSize,
    _Out_ PMDL *AudioBufferMdl,
    _Out_ ULONG *ActualSize,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    ULONG allocationRequest;
    ULONG requestedNotificationCount;
    ULONGLONG periodAlignedSize;

    requestedNotificationCount = NotificationCount != 0 ? NotificationCount :
                                                          (VIOSND_DEFAULT_BUFFER_BYTES /
                                                           VIOSND_DEFAULT_PERIOD_BYTES);
    requestedNotificationCount = max(requestedNotificationCount, 1u);
    periodAlignedSize = (ULONGLONG)requestedNotificationCount * m_PeriodBytes;
    if (periodAlignedSize > MAXULONG) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    allocationRequest = (ULONG)periodAlignedSize;

    NTSTATUS status = AllocateAudioBuffer(allocationRequest,
                                          AudioBufferMdl,
                                          ActualSize,
                                          OffsetFromFirstPage,
                                          CacheType);
    if (NT_SUCCESS(status)) {
        m_NotificationCount = ViosndNotificationCountFromBufferSize(m_BufferSize, m_PeriodBytes);
        m_PacketSize = ViosndPacketSizeFromNotificationCount(m_BufferSize,
                                                             m_NotificationCount,
                                                             m_PeriodBytes);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u %s AllocateBufferWithNotification requested=%u allocatedRequest=%u actual=%u packet=%u notif=%u requestedNotif=%u fixedPeriod=%u\n",
                   m_StreamId,
                   m_Capture ? "capture" : "render",
                   RequestedSize,
                   allocationRequest,
                   m_BufferSize,
                   m_PacketSize,
                   m_NotificationCount,
                   NotificationCount,
                   VIOSND_DEFAULT_PERIOD_BYTES);
    }
    return status;
}

STDMETHODIMP_(VOID)
CViosndMiniportWaveRTStream::FreeBufferWithNotification(_In_ PMDL AudioBufferMdl, _In_ ULONG BufferSize)
{
    FreeAudioBuffer(AudioBufferMdl, BufferSize);
    m_NotificationCount = 0;
    m_PacketSize = 0;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::RegisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    m_NotificationEvent = NotificationEvent;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    if (m_NotificationEvent == NotificationEvent) {
        m_NotificationEvent = NULL;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetReadPacket(
    _Out_ ULONG *PacketNumber,
    _Out_ DWORD *Flags,
    _Out_ ULONG64 *PerformanceCounterValue,
    _Out_ BOOL *MoreData)
{
    LARGE_INTEGER qpc;

    if (!m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (m_State != KSSTATE_RUN || m_OutstandingWrites == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    *PacketNumber = m_NextReadPacket++;
    *Flags = 0;
    qpc = KeQueryPerformanceCounter(NULL);
    *PerformanceCounterValue = (ULONG64)qpc.QuadPart;
    m_OutstandingWrites--;
    *MoreData = m_OutstandingWrites != 0 ? TRUE : FALSE;
    m_CaptureReadPackets++;
    if (m_CaptureReadPackets <= 16 || ((*PacketNumber) & 0x7f) == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u capture GetReadPacket count=%u packet=%u ready=%u more=%u qpc=%llu\n",
                   m_StreamId,
                   m_CaptureReadPackets,
                   *PacketNumber,
                   m_OutstandingWrites,
                   *MoreData,
                   *PerformanceCounterValue);
    }
    if (m_NotificationEvent != NULL) {
        KeSetEvent(m_NotificationEvent, IO_NO_INCREMENT, FALSE);
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::SetWritePacket(
    _In_ ULONG PacketNumber,
    _In_ DWORD Flags,
    _In_ ULONG EosPacketLength)
{
    if (m_Capture || m_Buffer == NULL || m_PacketSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (m_LastOsWritePacket == MAXULONG) {
        m_NextSubmitPacket = PacketNumber;
    }
    m_LastOsWritePacket = PacketNumber;

    if ((Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) != 0) {
        m_EosPacketNumber = PacketNumber;
        m_EosPacketLength = min(EosPacketLength, m_PacketSize);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render SetWritePacket EOS packet=%u eosLength=%u packetSize=%u outstanding=%u next=%u\n",
                   m_StreamId,
                   PacketNumber,
                   m_EosPacketLength,
                   m_PacketSize,
                   m_OutstandingWrites,
                   m_NextSubmitPacket);
    } else if (m_EosPacketNumber != MAXULONG &&
               ViosndPacketNumberLessOrEqual(m_EosPacketNumber, PacketNumber)) {
        m_EosPacketNumber = MAXULONG;
        m_EosPacketLength = 0;
    }

    if ((PacketNumber & VIOSND_PERIODIC_LOG_MASK) == 0 || m_OutstandingWrites == 0) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: stream %u render SetWritePacket packet=%u flags=0x%x last=%u next=%u outstanding=%u state=%u\n",
                   m_StreamId,
                   PacketNumber,
                   Flags,
                   m_LastOsWritePacket,
                   m_NextSubmitPacket,
                   m_OutstandingWrites,
                   m_State);
    }

    KeSetEvent(&m_KickEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetOutputStreamPresentationPosition(
    _Out_ KSAUDIO_PRESENTATION_POSITION *PresentationPosition)
{
    ULONGLONG currentPosition = m_Capture ? m_Position : GetRenderClockPosition();
    ULONG frameBytes = ViosndFrameBytes(&m_WaveFormat);

    if (frameBytes == 0) {
        frameBytes = (VIOSND_DEFAULT_CHANNELS * VIOSND_DEFAULT_BITS_PER_SAMPLE) / 8;
    }
    /* A block is a frame of the format this stream negotiated. Dividing a 32-bit stream's byte
     * position by a 16-bit frame reports twice as many frames as have played. */
    PresentationPosition->u64PositionInBlocks = currentPosition / frameBytes;
    PresentationPosition->u64QPCPosition = KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRTStream::GetPacketCount(_Out_ ULONG *PacketCount)
{
    *PacketCount = m_PacketNumber;
    return STATUS_SUCCESS;
}

CViosndMiniportWaveRT::CViosndMiniportWaveRT(
    _In_ PVIOSND_DEVICE Device,
    _In_ const VIOSND_ENDPOINT *Endpoint) :
    m_RefCount(1),
    m_Device(Device),
    m_StreamId(Endpoint->StreamId),
    m_Capture(Endpoint->Capture),
    m_Port(NULL),
    m_RangePointers(NULL),
    m_RangeCount(0)
{
    m_Endpoint = *Endpoint;
    RtlZeroMemory(m_Pins, sizeof(m_Pins));
    RtlZeroMemory(&m_Filter, sizeof(m_Filter));
}

NTSTATUS
CViosndMiniportWaveRT::BuildDescription()
{
    const PCFILTER_DESCRIPTOR *tmpl = m_Capture ? &ViosndCaptureFilterDescriptor
                                                : &ViosndRenderFilterDescriptor;
    NTSTATUS status;

    /* virtio-snd allows a channel count far past anything Windows will route here, and the
     * advertised maximum is what the engine believes it may ask for. Cap it before it is
     * published rather than refusing the request afterwards. */
    if (m_Endpoint.Caps.ChannelsMax > VIOSND_MAX_CHANNELS) {
        m_Endpoint.Caps.ChannelsMax = (UCHAR)VIOSND_MAX_CHANNELS;
    }
    if (m_Endpoint.Caps.ChannelsMin > m_Endpoint.Caps.ChannelsMax) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (m_Endpoint.Preferred.Channels > m_Endpoint.Caps.ChannelsMax) {
        m_Endpoint.Preferred.Channels = m_Endpoint.Caps.ChannelsMax;
    }

    status = ViosndBuildDataRanges(&m_Endpoint.Caps,
                                   &m_Endpoint.Preferred,
                                   &m_RangePointers,
                                   &m_RangeCount);
    if (!NT_SUCCESS(status)) {
        /* The endpoint offered nothing Windows can express. Say so rather than falling back to
         * a format the host never claimed to accept. */
        ViosndRecordDiag(m_Device,
                         m_Capture ? L"CaptureRanges" : L"RenderRanges",
                         L"no expressible format");
        return status;
    }

    RtlCopyMemory(m_Pins, tmpl->Pins, sizeof(m_Pins));
    m_Pins[VIOSND_PIN_SYSTEM].KsPinDescriptor.DataRanges = m_RangePointers;
    m_Pins[VIOSND_PIN_SYSTEM].KsPinDescriptor.DataRangesCount = m_RangeCount;

    m_Filter = *tmpl;
    m_Filter.Pins = m_Pins;
    m_Filter.PinCount = SIZEOF_ARRAY(m_Pins);

    {
        WCHAR text[224];

        if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                           SIZEOF_ARRAY(text),
                                           L"stream=%u nid=%u kind=%u ranges=%u "
                                           L"preferred=%uHz/%uch/%ubit%s%s",
                                           m_Endpoint.StreamId,
                                           m_Endpoint.DeviceIndex,
                                           m_Endpoint.Kind,
                                           m_RangeCount,
                                           m_Endpoint.Preferred.SampleRate,
                                           m_Endpoint.Preferred.Channels,
                                           m_Endpoint.Preferred.ContainerBits,
                                           m_Endpoint.Preferred.Float ? L" float" : L"",
                                           m_Endpoint.PreferredFromHost ? L" (host)"
                                                                        : L" (driver)"))) {
            ViosndRecordDiag(m_Device,
                             m_Capture ? L"CaptureRanges" : L"RenderRanges",
                             text);
        }
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::QueryInterface(_In_ REFIID InterfaceId, _COM_Outptr_ PVOID *Interface)
{
    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniport) ||
        IsEqualGUIDAligned(InterfaceId, IID_IMiniportWaveRT)) {
        *Interface = (IMiniportWaveRT *)this;
    }

    if (*Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRT::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG)
CViosndMiniportWaveRT::Release()
{
    LONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        /* Freed here rather than from a destructor: declaring one makes the compiler emit a
         * deleting destructor, and that references an operator delete a kernel driver has no
         * reason to carry. The stream class next door is built the same way. */
        ViosndFreeDataRanges(m_RangePointers);
        m_RangePointers = NULL;
        m_RangeCount = 0;
        this->~CViosndMiniportWaveRT();
        ExFreePoolWithTag(this, VIOSND_POOL_TAG);
    }
    return (ULONG)count;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &m_Filter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    const KSDATARANGE_AUDIO *ours;
    const KSDATARANGE_AUDIO *theirs;
    VIOSND_WAVE_FORMAT wave;
    WAVEFORMATEXTENSIBLE wfx;
    ULONG channels;

    /* Only the streaming pin carries a format; the bridge pin is analog and has no intersection
     * to compute. */
    if (PinId != VIOSND_PIN_SYSTEM) {
        return STATUS_NO_MATCH;
    }

    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    if (OutputBufferLength == 0) {
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (MatchingDataRange == NULL ||
        MatchingDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO) ||
        DataRange == NULL ||
        !IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    /* Our own ranges are single points -- one rate, one width each -- so the intersection is
     * that point, provided the caller's range admits it. Channels are the one axis with room to
     * negotiate, and there the narrower of the two wins. */
    ours = (const KSDATARANGE_AUDIO *)MatchingDataRange;
    channels = ours->MaximumChannels;

    if (DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
        theirs = (const KSDATARANGE_AUDIO *)DataRange;
        if (ours->MinimumSampleFrequency < theirs->MinimumSampleFrequency ||
            ours->MinimumSampleFrequency > theirs->MaximumSampleFrequency ||
            ours->MinimumBitsPerSample < theirs->MinimumBitsPerSample ||
            ours->MinimumBitsPerSample > theirs->MaximumBitsPerSample) {
            return STATUS_NO_MATCH;
        }
        if (theirs->MaximumChannels != 0 && theirs->MaximumChannels < channels) {
            channels = theirs->MaximumChannels;
        }
    }

    if (channels == 0 || channels > VIOSND_MAX_CHANNELS) {
        return STATUS_NO_MATCH;
    }

    /* Round-trip it through the stream's own capabilities rather than trusting the arithmetic:
     * whatever comes back is a format the device has said it will accept. */
    RtlZeroMemory(&wfx, sizeof(wfx));
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = (WORD)channels;
    wfx.Format.nSamplesPerSec = ours->MinimumSampleFrequency;
    wfx.Format.wBitsPerSample = (WORD)ours->MinimumBitsPerSample;
    wfx.Format.nBlockAlign = (WORD)((channels * ours->MinimumBitsPerSample) / 8);
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;
    wfx.dwChannelMask = ViosndChannelMask((UCHAR)channels);
    wfx.SubFormat = ours->DataRange.SubFormat;

    if (!ViosndFormatFromWave(&wfx.Format, &m_Endpoint.Caps, &wave)) {
        return STATUS_NO_MATCH;
    }

    ViosndBuildWaveFormat(&wave, (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)ResultantFormat);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    m_Port = Port;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM *Stream,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat)
{
    UNREFERENCED_PARAMETER(PortStream);

    /*
     * Four ways to refuse, one status for all of them, and the caller is the OS -- which turns
     * the refusal into an endpoint that opens and returns silence. Record which one it was, and
     * what was asked for, before returning.
     */
    VIOSND_WAVE_FORMAT wave;
    BOOLEAN formatOk = ViosndResolveDataFormat(DataFormat, &m_Endpoint.Caps, &wave);

    {
        WCHAR text[224];
        PWAVEFORMATEX wfx = NULL;

        if (DataFormat != NULL &&
            DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX) &&
            IsEqualGUIDAligned(DataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
            wfx = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
        }
        if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                           SIZEOF_ARRAY(text),
                                           L"%s pin=%u(want %u) capture=%u(want %u) format=%u"
                                           L" tag=%u ch=%u rate=%u bits=%u",
                                           (Stream != NULL && Pin == VIOSND_PIN_SYSTEM &&
                                            Capture == m_Capture && formatOk)
                                               ? L"accepted:"
                                               : L"refused:",
                                           Pin,
                                           VIOSND_PIN_SYSTEM,
                                           Capture,
                                           m_Capture,
                                           formatOk,
                                           wfx != NULL ? wfx->wFormatTag : 0,
                                           wfx != NULL ? wfx->nChannels : 0,
                                           wfx != NULL ? wfx->nSamplesPerSec : 0,
                                           wfx != NULL ? wfx->wBitsPerSample : 0))) {
            ViosndRecordDiag(m_Device,
                             m_Capture ? L"CaptureNewStream" : L"RenderNewStream",
                             text);
        }
    }

    if (Stream == NULL ||
        Pin != VIOSND_PIN_SYSTEM ||
        Capture != m_Capture ||
        !formatOk) {
        return STATUS_INVALID_PARAMETER;
    }

    PVOID memory = ExAllocatePoolUninitialized(NonPagedPoolNx,
                                               sizeof(CViosndMiniportWaveRTStream),
                                               VIOSND_POOL_TAG);
    if (memory == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Stream = new(memory) CViosndMiniportWaveRTStream(this,
                                                      m_Device,
                                                      m_StreamId,
                                                      m_Capture,
                                                      &wave);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CViosndMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = FALSE;
    DeviceDescription->InterfaceType = PCIBus;
    DeviceDescription->DmaWidth = Width32Bits;
    DeviceDescription->DmaSpeed = Compatible;
    DeviceDescription->MaximumLength = VIOSND_DEFAULT_BUFFER_BYTES;
    return STATUS_SUCCESS;
}

NTSTATUS
ViosndCreateWaveRTMiniport(
    _In_ PVIOSND_DEVICE Device,
    _In_ const VIOSND_ENDPOINT *Endpoint,
    _Outptr_ PMINIPORT *Miniport)
{
    PVOID memory;
    CViosndMiniportWaveRT *miniport;
    NTSTATUS status;

    *Miniport = NULL;
    memory = ExAllocatePoolUninitialized(NonPagedPoolNx,
                                         sizeof(CViosndMiniportWaveRT),
                                         VIOSND_POOL_TAG);
    if (memory == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    miniport = new(memory) CViosndMiniportWaveRT(Device, Endpoint);
    status = miniport->BuildDescription();
    if (!NT_SUCCESS(status)) {
        miniport->Release();
        return status;
    }

    *Miniport = miniport;
    return STATUS_SUCCESS;
}


