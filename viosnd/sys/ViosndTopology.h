#pragma once

#define VIOSND_TOPOOUT_NAME L"XCBVirtioAudioRenderTopoA"
#define VIOSND_TOPOIN_NAME  L"XCBVirtioAudioCaptureTopoA"

enum {
    VIOSND_TOPO_PIN_SOURCE = 0,
    VIOSND_TOPO_PIN_BRIDGE = 1
};

NTSTATUS
ViosndCreateTopologyMiniport(
    _In_ BOOLEAN Capture,
    _Outptr_ PMINIPORT *Miniport);
