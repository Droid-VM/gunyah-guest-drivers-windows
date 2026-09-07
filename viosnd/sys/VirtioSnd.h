#pragma once

#include "..\..\VirtIO\linux\types.h"

enum {
    VIRTIO_SND_F_CTLS = 0
};

typedef struct virtio_snd_config {
    u32 jacks;
    u32 streams;
    u32 chmaps;
    u32 controls;
} VIRTIO_SND_CONFIG, *PVIRTIO_SND_CONFIG;

/*
 * DroidVM vendor block, published by crosvm immediately after the spec's config.
 *
 * The spec's own layout ends at 16 bytes (the fourth u32 is `controls`, valid only with
 * VIRTIO_SND_F_CTLS and zero otherwise), so a vendor is free to build from there. It carries the
 * settings that belong to the guest driver but are chosen host-side -- how much audio to keep in
 * flight is a latency decision the user makes in the app, not something a driver can know.
 *
 * Absent or mismatched magic means an ordinary virtio-snd device: keep the built-in defaults.
 */
#define VIOSND_VENDOR_CFG_OFFSET  64u          /* not 16: leave the spec room to grow */
#define VIOSND_VENDOR_CFG_MAGIC   0x534d5644u  /* "DVMS" */
/* Versions only ever append fields, so a driver reads the prefix its own version covers and
 * ignores the rest. Checking for equality instead would make every host-side addition look like
 * "no vendor block at all" to an older driver, and it would lose the settings it does
 * understand -- which is worse than the addition it does not. */
#define VIOSND_VENDOR_CFG_MIN_VERSION 1u
#define VIOSND_VENDOR_CFG_VERSION     2u

/* Per-direction cap on the preferred-format hints, matching the host's block. */
#define VIOSND_VENDOR_CFG_MAX_DEVICES 8u

/* What sort of thing a host endpoint is. Deliberately coarse: it has to mean the same to every
 * guest, so it names what a listener would recognise rather than the host's device taxonomy.
 * virtio-snd's own jacks cannot carry this -- they describe connectors on the emulated card,
 * not the host endpoint behind it. */
enum {
    VIOSND_ENDPOINT_KIND_UNKNOWN = 0,
    VIOSND_ENDPOINT_KIND_SPEAKER = 1,
    VIOSND_ENDPOINT_KIND_HEADPHONES = 2,
    VIOSND_ENDPOINT_KIND_HEADSET = 3,
    VIOSND_ENDPOINT_KIND_LINE_OUT = 4,
    VIOSND_ENDPOINT_KIND_DIGITAL = 5,
    VIOSND_ENDPOINT_KIND_MICROPHONE = 6,
    VIOSND_ENDPOINT_KIND_TELEPHONY = 7
};

typedef struct viosnd_vendor_preferred {
    u32 rate;     /* the host endpoint's own sample rate; 0 = unknown */
    u32 channels; /* the host endpoint's own channel count; 0 = unknown */
    u32 kind;     /* VIOSND_ENDPOINT_KIND_*; 0 = unknown */
} VIOSND_VENDOR_PREFERRED, *PVIOSND_VENDOR_PREFERRED;

/* Version 1 stops after period_bytes. */
typedef struct viosnd_vendor_config {
    u32 magic;
    u32 version;
    u32 outstanding_packets; /* periods to keep in flight; 0 = driver default */
    u32 period_bytes;        /* preferred period size; 0 = no preference */
    /* Version 2 onwards. Indexed by hda_fn_nid, separately per direction because output device
     * 0 and input device 0 both report nid 0. */
    u32 preferred_output_count;
    u32 preferred_input_count;
    VIOSND_VENDOR_PREFERRED preferred_output[VIOSND_VENDOR_CFG_MAX_DEVICES];
    VIOSND_VENDOR_PREFERRED preferred_input[VIOSND_VENDOR_CFG_MAX_DEVICES];
} VIOSND_VENDOR_CONFIG, *PVIOSND_VENDOR_CONFIG;

#define VIOSND_VENDOR_CFG_V1_SIZE \
    (FIELD_OFFSET(VIOSND_VENDOR_CONFIG, period_bytes) + sizeof(u32))

enum {
    VIRTIO_SND_VQ_CONTROL = 0,
    VIRTIO_SND_VQ_EVENT,
    VIRTIO_SND_VQ_TX,
    VIRTIO_SND_VQ_RX,
    VIRTIO_SND_VQ_MAX
};

enum {
    VIRTIO_SND_D_OUTPUT = 0,
    VIRTIO_SND_D_INPUT
};

enum {
    VIRTIO_SND_R_JACK_INFO = 1,
    VIRTIO_SND_R_JACK_REMAP,

    VIRTIO_SND_R_PCM_INFO = 0x0100,
    VIRTIO_SND_R_PCM_SET_PARAMS,
    VIRTIO_SND_R_PCM_PREPARE,
    VIRTIO_SND_R_PCM_RELEASE,
    VIRTIO_SND_R_PCM_START,
    VIRTIO_SND_R_PCM_STOP,

    VIRTIO_SND_R_CHMAP_INFO = 0x0200,

    VIRTIO_SND_EVT_JACK_CONNECTED = 0x1000,
    VIRTIO_SND_EVT_JACK_DISCONNECTED,
    VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED = 0x1100,
    VIRTIO_SND_EVT_PCM_XRUN,

    VIRTIO_SND_S_OK = 0x8000,
    VIRTIO_SND_S_BAD_MSG,
    VIRTIO_SND_S_NOT_SUPP,
    VIRTIO_SND_S_IO_ERR
};

typedef struct virtio_snd_hdr {
    u32 code;
} VIRTIO_SND_HDR, *PVIRTIO_SND_HDR;

typedef struct virtio_snd_event {
    VIRTIO_SND_HDR hdr;
    u32 data;
} VIRTIO_SND_EVENT, *PVIRTIO_SND_EVENT;

typedef struct virtio_snd_query_info {
    VIRTIO_SND_HDR hdr;
    u32 start_id;
    u32 count;
    u32 size;
} VIRTIO_SND_QUERY_INFO, *PVIRTIO_SND_QUERY_INFO;

typedef struct virtio_snd_info {
    u32 hda_fn_nid;
} VIRTIO_SND_INFO, *PVIRTIO_SND_INFO;

typedef struct virtio_snd_pcm_hdr {
    VIRTIO_SND_HDR hdr;
    u32 stream_id;
} VIRTIO_SND_PCM_HDR, *PVIRTIO_SND_PCM_HDR;

enum {
    VIRTIO_SND_PCM_F_SHMEM_HOST = 0,
    VIRTIO_SND_PCM_F_SHMEM_GUEST,
    VIRTIO_SND_PCM_F_MSG_POLLING,
    VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS,
    VIRTIO_SND_PCM_F_EVT_XRUNS
};

enum {
    VIRTIO_SND_PCM_FMT_IMA_ADPCM = 0,
    VIRTIO_SND_PCM_FMT_MU_LAW,
    VIRTIO_SND_PCM_FMT_A_LAW,
    VIRTIO_SND_PCM_FMT_S8,
    VIRTIO_SND_PCM_FMT_U8,
    VIRTIO_SND_PCM_FMT_S16,
    VIRTIO_SND_PCM_FMT_U16,
    VIRTIO_SND_PCM_FMT_S18_3,
    VIRTIO_SND_PCM_FMT_U18_3,
    VIRTIO_SND_PCM_FMT_S20_3,
    VIRTIO_SND_PCM_FMT_U20_3,
    VIRTIO_SND_PCM_FMT_S24_3,
    VIRTIO_SND_PCM_FMT_U24_3,
    VIRTIO_SND_PCM_FMT_S20,
    VIRTIO_SND_PCM_FMT_U20,
    VIRTIO_SND_PCM_FMT_S24,
    VIRTIO_SND_PCM_FMT_U24,
    VIRTIO_SND_PCM_FMT_S32,
    VIRTIO_SND_PCM_FMT_U32,
    VIRTIO_SND_PCM_FMT_FLOAT,
    VIRTIO_SND_PCM_FMT_FLOAT64,
    VIRTIO_SND_PCM_FMT_DSD_U8,
    VIRTIO_SND_PCM_FMT_DSD_U16,
    VIRTIO_SND_PCM_FMT_DSD_U32,
    VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME
};

enum {
    VIRTIO_SND_PCM_RATE_5512 = 0,
    VIRTIO_SND_PCM_RATE_8000,
    VIRTIO_SND_PCM_RATE_11025,
    VIRTIO_SND_PCM_RATE_16000,
    VIRTIO_SND_PCM_RATE_22050,
    VIRTIO_SND_PCM_RATE_32000,
    VIRTIO_SND_PCM_RATE_44100,
    VIRTIO_SND_PCM_RATE_48000,
    VIRTIO_SND_PCM_RATE_64000,
    VIRTIO_SND_PCM_RATE_88200,
    VIRTIO_SND_PCM_RATE_96000,
    VIRTIO_SND_PCM_RATE_176400,
    VIRTIO_SND_PCM_RATE_192000,
    VIRTIO_SND_PCM_RATE_384000
};

typedef struct virtio_snd_pcm_info {
    VIRTIO_SND_INFO hdr;
    u32 features;
    u64 formats;
    u64 rates;
    u8 direction;
    u8 channels_min;
    u8 channels_max;
    u8 padding[5];
} VIRTIO_SND_PCM_INFO, *PVIRTIO_SND_PCM_INFO;

typedef struct virtio_snd_pcm_set_params {
    VIRTIO_SND_PCM_HDR hdr;
    u32 buffer_bytes;
    u32 period_bytes;
    u32 features;
    u8 channels;
    u8 format;
    u8 rate;
    u8 padding;
} VIRTIO_SND_PCM_SET_PARAMS, *PVIRTIO_SND_PCM_SET_PARAMS;

typedef struct virtio_snd_pcm_xfer {
    u32 stream_id;
} VIRTIO_SND_PCM_XFER, *PVIRTIO_SND_PCM_XFER;

typedef struct virtio_snd_pcm_status {
    u32 status;
    u32 latency_bytes;
} VIRTIO_SND_PCM_STATUS, *PVIRTIO_SND_PCM_STATUS;
