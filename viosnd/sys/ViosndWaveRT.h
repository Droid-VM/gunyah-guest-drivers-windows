#pragma once

/* The subdevice name is the endpoint's identity to Windows -- it is what the endpoint's registry
 * key is derived from, and that key holds the name and the format Windows settled on, neither of
 * which it will revisit. So the name carries no meaning: it is a slot letter, and everything the
 * user sees is published separately, where it can be changed without stranding an endpoint. */
#define VIOSND_WAVEOUT_NAME L"XCBVirtioAudioRenderA"
#define VIOSND_WAVEIN_NAME  L"XCBVirtioAudioCaptureA"

enum {
    VIOSND_PIN_SYSTEM = 0,
    VIOSND_PIN_BRIDGE = 1
};

NTSTATUS
ViosndCreateWaveRTMiniport(
    _In_ PVIOSND_DEVICE Device,
    _In_ const VIOSND_ENDPOINT *Endpoint,
    _Outptr_ PMINIPORT *Miniport);
