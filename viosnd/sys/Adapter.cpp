#include "precomp.h"

static PVIOSND_DEVICE g_ViosndDevice;

#define VIOSND_ENDPOINT_ROLE_RENDER  0u
#define VIOSND_ENDPOINT_ROLE_CAPTURE 1u
#define VIOSND_ENDPOINT_ROLE_BOTH    2u

typedef struct _VIOSND_SUBDEVICE {
    PPORT Port;
    PMINIPORT Miniport;
} VIOSND_SUBDEVICE, *PVIOSND_SUBDEVICE;

static
VOID
ViosndReleaseSubdevice(
    _Inout_ PVIOSND_SUBDEVICE Subdevice)
{
    if (Subdevice->Miniport != NULL) {
        Subdevice->Miniport->Release();
        Subdevice->Miniport = NULL;
    }
    if (Subdevice->Port != NULL) {
        Subdevice->Port->Release();
        Subdevice->Port = NULL;
    }
}

static
ULONG
ViosndReadDeviceDword(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_z_ PCWSTR ValueName,
    _In_ ULONG DefaultValue)
{
    HANDLE key = NULL;
    NTSTATUS status;
    UNICODE_STRING valueName;
    ULONG resultLength;
    ULONG value = DefaultValue;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION information;

    status = IoOpenDeviceRegistryKey(PhysicalDeviceObject,
                                     PLUGPLAY_REGKEY_DEVICE,
                                     KEY_READ,
                                     &key);
    if (!NT_SUCCESS(status)) {
        return DefaultValue;
    }

    RtlInitUnicodeString(&valueName, ValueName);
    information = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    RtlZeroMemory(buffer, sizeof(buffer));

    status = ZwQueryValueKey(key,
                             &valueName,
                             KeyValuePartialInformation,
                             information,
                             sizeof(buffer),
                             &resultLength);
    if (NT_SUCCESS(status) &&
        information->Type == REG_DWORD &&
        information->DataLength >= sizeof(ULONG)) {
        value = *((PULONG)information->Data);
    }

    ZwClose(key);
    return value;
}

/*
 * Records a line of driver state under the device's own registry key.
 *
 * The alternative is DbgPrint, which needs a kernel debugger attached to be visible at all --
 * so on a machine nobody is debugging, the driver's account of what it decided simply does not
 * exist. A value here can be read back from anywhere that can reach the registry, long after
 * the fact, which is what makes it useful for the questions that only come up later: which
 * version of the vendor block the host published, and what the driver did with it.
 */
VOID
ViosndWriteDeviceDiagString(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_z_ PCWSTR ValueName,
    _In_z_ PCWSTR Value)
{
    /*
     * Recording a diagnostic means opening a registry key, and that is PASSIVE_LEVEL only. Some
     * of what is worth recording happens on the audio engine's own path, which is not -- and a
     * driver that bugchecks while explaining itself is worse than one that says nothing. Callers
     * do not have to know which side of the line they are on.
     */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    HANDLE key = NULL;
    UNICODE_STRING name;
    UNICODE_STRING value;

    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(PhysicalDeviceObject,
                                            PLUGPLAY_REGKEY_DEVICE,
                                            KEY_SET_VALUE,
                                            &key))) {
        return;
    }
    RtlInitUnicodeString(&name, ValueName);
    RtlInitUnicodeString(&value, Value);
    (VOID)ZwSetValueKey(key, &name, 0, REG_SZ, value.Buffer, value.Length + sizeof(WCHAR));
    ZwClose(key);
}

static
VOID
ViosndWriteDeviceInitDiag(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_z_ PCWSTR Stage,
    _In_ NTSTATUS Status)
{
    HANDLE key = NULL;
    NTSTATUS openStatus;
    UNICODE_STRING valueName;
    UNICODE_STRING value;

    openStatus = IoOpenDeviceRegistryKey(PhysicalDeviceObject,
                                         PLUGPLAY_REGKEY_DEVICE,
                                         KEY_SET_VALUE,
                                         &key);
    if (!NT_SUCCESS(openStatus)) {
        return;
    }

    RtlInitUnicodeString(&valueName, L"LastInitStage");
    RtlInitUnicodeString(&value, Stage);
    (VOID)ZwSetValueKey(key,
                        &valueName,
                        0,
                        REG_SZ,
                        value.Buffer,
                        value.Length + sizeof(WCHAR));

    RtlInitUnicodeString(&valueName, L"LastInitStatus");
    (VOID)ZwSetValueKey(key,
                        &valueName,
                        0,
                        REG_DWORD,
                        &Status,
                        sizeof(Status));

    ZwClose(key);
}

/* Where a subdevice registration got to. A single returned status says a registration failed but
 * not which of the four calls did it, and PortCls returns the same codes from all of them. */
typedef struct _VIOSND_REGISTER_STEPS {
    NTSTATUS NewPort;
    NTSTATUS Miniport;
    NTSTATUS Init;
    NTSTATUS Register;
} VIOSND_REGISTER_STEPS, *PVIOSND_REGISTER_STEPS;

static
NTSTATUS
ViosndCreateAndRegisterWaveRTSubdevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PVIOSND_DEVICE Device,
    _In_ const VIOSND_ENDPOINT *Endpoint,
    _In_ PWSTR Name,
    _Out_ PVIOSND_SUBDEVICE Subdevice,
    _Out_ PVIOSND_REGISTER_STEPS Steps)
{
    NTSTATUS status;

    RtlZeroMemory(Subdevice, sizeof(*Subdevice));
    RtlZeroMemory(Steps, sizeof(*Steps));

    status = PcNewPort(&Subdevice->Port, CLSID_PortWaveRT);
    Steps->NewPort = status;
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PcNewPort(WaveRT %ws) failed 0x%08x\n",
                   Name,
                   status);
        return status;
    }

    status = ViosndCreateWaveRTMiniport(Device, Endpoint, &Subdevice->Miniport);
    Steps->Miniport = status;
    if (NT_SUCCESS(status)) {
        status = Subdevice->Port->Init(DeviceObject,
                                       Irp,
                                       Subdevice->Miniport,
                                       NULL,
                                       ResourceList);
        Steps->Init = status;
    }
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: WaveRT Init %ws failed 0x%08x\n",
                   Name,
                   status);
    }

    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, Name, Subdevice->Port);
        Steps->Register = status;
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: PcRegisterSubdevice(WaveRT %ws) failed 0x%08x\n",
                       Name,
                       status);
        }
    }

    if (!NT_SUCCESS(status)) {
        ViosndReleaseSubdevice(Subdevice);
    }
    return status;
}

static
NTSTATUS
ViosndCreateAndRegisterTopologySubdevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ const VIOSND_ENDPOINT *Endpoint,
    _In_ PWSTR Name,
    _Out_ PVIOSND_SUBDEVICE Subdevice,
    _Out_ PVIOSND_REGISTER_STEPS Steps)
{
    NTSTATUS status;

    RtlZeroMemory(Subdevice, sizeof(*Subdevice));
    RtlZeroMemory(Steps, sizeof(*Steps));

    status = PcNewPort(&Subdevice->Port, CLSID_PortTopology);
    Steps->NewPort = status;
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PcNewPort(Topology %ws) failed 0x%08x\n",
                   Name,
                   status);
        return status;
    }

    status = ViosndCreateTopologyMiniport(Endpoint->Capture, &Subdevice->Miniport);
    Steps->Miniport = status;
    if (NT_SUCCESS(status)) {
        status = Subdevice->Port->Init(DeviceObject,
                                       Irp,
                                       Subdevice->Miniport,
                                       NULL,
                                       ResourceList);
        Steps->Init = status;
    }
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: Topology Init %ws failed 0x%08x\n",
                   Name,
                   status);
    }

    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, Name, Subdevice->Port);
        Steps->Register = status;
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: PcRegisterSubdevice(Topology %ws) failed 0x%08x\n",
                       Name,
                       status);
        }
    }

    if (!NT_SUCCESS(status)) {
        ViosndReleaseSubdevice(Subdevice);
    }
    return status;
}

static
NTSTATUS
ViosndRegisterAudioEndpoint(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PVIOSND_DEVICE Device,
    _In_ const VIOSND_ENDPOINT *Endpoint,
    _In_ PWSTR TopologyName,
    _In_ PWSTR WaveName,
    _Out_ PVIOSND_REGISTER_STEPS TopologySteps,
    _Out_ PVIOSND_REGISTER_STEPS WaveSteps,
    _Out_ PNTSTATUS ConnectionStatus)
{
    NTSTATUS status;
    VIOSND_SUBDEVICE topology;
    VIOSND_SUBDEVICE wave;

    RtlZeroMemory(&topology, sizeof(topology));
    RtlZeroMemory(&wave, sizeof(wave));
    RtlZeroMemory(TopologySteps, sizeof(*TopologySteps));
    RtlZeroMemory(WaveSteps, sizeof(*WaveSteps));
    *ConnectionStatus = STATUS_SUCCESS;

    status = ViosndCreateAndRegisterTopologySubdevice(DeviceObject,
                                                      Irp,
                                                      ResourceList,
                                                      Endpoint,
                                                      TopologyName,
                                                      &topology,
                                                      TopologySteps);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ViosndCreateAndRegisterWaveRTSubdevice(DeviceObject,
                                                    Irp,
                                                    ResourceList,
                                                    Device,
                                                    Endpoint,
                                                    WaveName,
                                                    &wave,
                                                    WaveSteps);
    if (NT_SUCCESS(status)) {
        if (Endpoint->Capture) {
            status = PcRegisterPhysicalConnection(DeviceObject,
                                                  topology.Port,
                                                  VIOSND_TOPO_PIN_BRIDGE,
                                                  wave.Port,
                                                  VIOSND_PIN_BRIDGE);
        } else {
            status = PcRegisterPhysicalConnection(DeviceObject,
                                                  wave.Port,
                                                  VIOSND_PIN_BRIDGE,
                                                  topology.Port,
                                                  VIOSND_TOPO_PIN_SOURCE);
        }
        *ConnectionStatus = status;
        if (!NT_SUCCESS(status)) {
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: PcRegisterPhysicalConnection %ws/%ws failed 0x%08x\n",
                       WaveName,
                       TopologyName,
                       status);
        }
    }

    ViosndReleaseSubdevice(&wave);
    ViosndReleaseSubdevice(&topology);
    return status;
}

/* How many of the endpoints the host offered actually became something Windows can see. The two
 * numbers differing is the whole point of recording it: a dropped endpoint is otherwise
 * indistinguishable from a host that never offered one. */
static VOID
ViosndWriteEndpointCountDiag(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_z_ PCWSTR ValueName,
    _In_ ULONG Registered,
    _In_ ULONG Offered)
{
    WCHAR text[64];

    if (NT_SUCCESS(RtlStringCchPrintfW(text,
                                       SIZEOF_ARRAY(text),
                                       L"%u of %u registered",
                                       Registered,
                                       Offered))) {
        ViosndWriteDeviceDiagString(PhysicalDeviceObject, ValueName, text);
    }
}

/*
 * The names Windows binds a subdevice to. They come from static strings in the INF, so this
 * table and the INF have to agree, and its length is what actually limits how many endpoints a
 * card can show.
 *
 * They are slot letters and mean nothing on purpose. Windows derives an endpoint's identity from
 * this name and remembers what it settled on against it -- the volume, whether it is the default,
 * which applications were routed to it -- and never revisits that. So changing a name costs every
 * installed machine those settings once, which is a reason for the name never to carry anything
 * that might want to change.
 */
static PCWSTR const ViosndRenderWaveNames[] = {
    VIOSND_WAVEOUT_NAME,
    L"XCBVirtioAudioRenderB",
    L"XCBVirtioAudioRenderC",
    L"XCBVirtioAudioRenderD"
};

static PCWSTR const ViosndRenderTopologyNames[] = {
    VIOSND_TOPOOUT_NAME,
    L"XCBVirtioAudioRenderTopoB",
    L"XCBVirtioAudioRenderTopoC",
    L"XCBVirtioAudioRenderTopoD"
};

static PCWSTR const ViosndCaptureWaveNames[] = {
    VIOSND_WAVEIN_NAME,
    L"XCBVirtioAudioCaptureB",
    L"XCBVirtioAudioCaptureC",
    L"XCBVirtioAudioCaptureD"
};

static PCWSTR const ViosndCaptureTopologyNames[] = {
    VIOSND_TOPOIN_NAME,
    L"XCBVirtioAudioCaptureTopoB",
    L"XCBVirtioAudioCaptureTopoC",
    L"XCBVirtioAudioCaptureTopoD"
};

/* Two subdevices per endpoint, both directions. Registering past the adapter's budget fails at
 * the fifth call with a status that says nothing about the cause. */
C_ASSERT(VIOSND_MAX_SUBDEVICES >= VIOSND_MAX_ENDPOINTS * 2 * 2);

C_ASSERT(SIZEOF_ARRAY(ViosndRenderWaveNames) == VIOSND_MAX_ENDPOINTS);
C_ASSERT(SIZEOF_ARRAY(ViosndRenderTopologyNames) == VIOSND_MAX_ENDPOINTS);
C_ASSERT(SIZEOF_ARRAY(ViosndCaptureWaveNames) == VIOSND_MAX_ENDPOINTS);
C_ASSERT(SIZEOF_ARRAY(ViosndCaptureTopologyNames) == VIOSND_MAX_ENDPOINTS);

/*
 * Registers every endpoint in one direction. An endpoint that fails to register is reported and
 * skipped rather than taking the others down with it: one host device that offered a format
 * Windows cannot express is not a reason for the card to have no audio at all. Only having
 * nothing left at the end is a failure.
 */
static NTSTATUS
ViosndRegisterDirection(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PVIOSND_DEVICE Device,
    _In_reads_(Count) const VIOSND_ENDPOINT *Endpoints,
    _In_ ULONG Count,
    _In_ PCWSTR const *TopologyNames,
    _In_ PCWSTR const *WaveNames,
    _Out_ PULONG Registered)
{
    NTSTATUS lastFailure = STATUS_SUCCESS;

    *Registered = 0;
    if (Count > VIOSND_MAX_ENDPOINTS) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: %u endpoints offered, %u names available; the rest are dropped\n",
                   Count,
                   VIOSND_MAX_ENDPOINTS);
        Count = VIOSND_MAX_ENDPOINTS;
    }

    for (ULONG i = 0; i < Count; ++i) {
        VIOSND_REGISTER_STEPS topologySteps;
        VIOSND_REGISTER_STEPS waveSteps;
        NTSTATUS connection = STATUS_SUCCESS;
        NTSTATUS status = ViosndRegisterAudioEndpoint(DeviceObject,
                                                      Irp,
                                                      ResourceList,
                                                      Device,
                                                      &Endpoints[i],
                                                      (PWSTR)TopologyNames[i],
                                                      (PWSTR)WaveNames[i],
                                                      &topologySteps,
                                                      &waveSteps,
                                                      &connection);
        {
            WCHAR name[32];
            WCHAR text[224];

            if (NT_SUCCESS(RtlStringCchPrintfW(name,
                                               SIZEOF_ARRAY(name),
                                               L"Register%s%u",
                                               Endpoints[i].Capture ? L"In" : L"Out",
                                               i)) &&
                NT_SUCCESS(RtlStringCchPrintfW(
                    text,
                    SIZEOF_ARRAY(text),
                    L"topo port=0x%08x mini=0x%08x init=0x%08x reg=0x%08x | "
                    L"wave port=0x%08x mini=0x%08x init=0x%08x reg=0x%08x | conn=0x%08x",
                    topologySteps.NewPort,
                    topologySteps.Miniport,
                    topologySteps.Init,
                    topologySteps.Register,
                    waveSteps.NewPort,
                    waveSteps.Miniport,
                    waveSteps.Init,
                    waveSteps.Register,
                    connection))) {
                ViosndRecordDiag(Device, name, text);
            }
        }
        if (NT_SUCCESS(status)) {
            (*Registered)++;
        } else {
            lastFailure = status;
            VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viosnd: endpoint %u (%ws) failed to register 0x%08x\n",
                       i,
                       WaveNames[i],
                       status);
        }
    }

    return *Registered != 0 ? STATUS_SUCCESS : lastFailure;
}


extern "C"
NTSTATUS
XcbVirtioAudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList)
{
    NTSTATUS status;
    PVIOSND_DEVICE device;
    PDEVICE_OBJECT physicalDeviceObject;
    VIOSND_ENDPOINT_SET endpoints;
    ULONG endpointRole;
    BOOLEAN enableRender;
    BOOLEAN enableCapture;

    status = PcGetPhysicalDeviceObject(DeviceObject, &physicalDeviceObject);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: PcGetPhysicalDeviceObject failed 0x%08x\n",
                   status);
        return status;
    }
    ViosndWriteDeviceInitDiag(physicalDeviceObject, L"Start", STATUS_PENDING);

    endpointRole = ViosndReadDeviceDword(physicalDeviceObject,
                                         L"EndpointRole",
                                         VIOSND_ENDPOINT_ROLE_BOTH);
    enableRender = endpointRole != VIOSND_ENDPOINT_ROLE_CAPTURE;
    enableCapture = endpointRole != VIOSND_ENDPOINT_ROLE_RENDER;

    VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
               DPFLTR_ERROR_LEVEL,
               "viosnd: endpoint role=%u render=%u capture=%u\n",
               endpointRole,
               enableRender,
               enableCapture);

    status = ViosndCreateDevice(DeviceObject, physicalDeviceObject, ResourceList, &device);
    ViosndWriteDeviceInitDiag(physicalDeviceObject, L"CreateDevice", status);
    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: ViosndCreateDevice failed 0x%08x\n",
                   status);
        return status;
    }

    status = ViosndInitializeDevice(device);
    ViosndWriteDeviceInitDiag(physicalDeviceObject, L"InitializeDevice", status);
    if (NT_SUCCESS(status)) {
        status = ViosndEnumerateEndpoints(device, &endpoints);
        ViosndWriteDeviceInitDiag(physicalDeviceObject, L"EnumerateEndpoints", status);
    }

    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: init/query streams failed 0x%08x\n",
                   status);
        ViosndDestroyDevice(device);
        return status;
    }

    // EndpointRole is a preference, not a promise. A virtio-snd card carries whatever directions
    // the host gave it, and one direction is an ordinary configuration -- DroidVM makes a card per
    // direction so that each can be pinned to its own host endpoint. Expose the intersection of
    // what was asked for and what exists; only having nothing left to expose is a failure.
    if (NT_SUCCESS(status)) {
        enableRender = enableRender && endpoints.RenderCount != 0;
        enableCapture = enableCapture && endpoints.CaptureCount != 0;
        if (!enableRender && !enableCapture) {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            ViosndWriteDeviceInitDiag(physicalDeviceObject, L"NoUsableStream", status);
        }
    }

    if (NT_SUCCESS(status)) {
        ViosndWriteDeviceInitDiag(physicalDeviceObject, L"PcmConfigureDeferred", status);
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: defer PCM configure render=%u/%u capture=%u/%u\n",
                   endpoints.RenderCount,
                   endpoints.RenderCount != 0 ? endpoints.Render[0].StreamId : 0,
                   endpoints.CaptureCount,
                   endpoints.CaptureCount != 0 ? endpoints.Capture[0].StreamId : 0);
    }

    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: configure PCM failed 0x%08x render=%u/%u capture=%u/%u\n",
                   status,
                   endpoints.RenderCount,
                   endpoints.RenderCount != 0 ? endpoints.Render[0].StreamId : 0,
                   endpoints.CaptureCount,
                   endpoints.CaptureCount != 0 ? endpoints.Capture[0].StreamId : 0);
        ViosndDestroyDevice(device);
        return status;
    }

    if (enableRender && endpoints.RenderCount != 0) {
        ULONG registered = 0;

        status = ViosndRegisterDirection(DeviceObject,
                                         Irp,
                                         ResourceList,
                                         device,
                                         endpoints.Render,
                                         endpoints.RenderCount,
                                         ViosndRenderTopologyNames,
                                         ViosndRenderWaveNames,
                                         &registered);
        ViosndWriteDeviceInitDiag(physicalDeviceObject, L"RegisterRender", status);
        ViosndWriteEndpointCountDiag(physicalDeviceObject,
                                     L"RenderEndpoints",
                                     registered,
                                     endpoints.RenderCount);
    }

    if (NT_SUCCESS(status) && enableCapture && endpoints.CaptureCount != 0) {
        ULONG registered = 0;

        status = ViosndRegisterDirection(DeviceObject,
                                         Irp,
                                         ResourceList,
                                         device,
                                         endpoints.Capture,
                                         endpoints.CaptureCount,
                                         ViosndCaptureTopologyNames,
                                         ViosndCaptureWaveNames,
                                         &registered);
        ViosndWriteDeviceInitDiag(physicalDeviceObject, L"RegisterCapture", status);
        ViosndWriteEndpointCountDiag(physicalDeviceObject,
                                     L"CaptureEndpoints",
                                     registered,
                                     endpoints.CaptureCount);
    }

    if (!NT_SUCCESS(status)) {
        VIOSND_LOG(DPFLTR_IHVDRIVER_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viosnd: register audio endpoint failed 0x%08x\n",
                   status);
        ViosndDestroyDevice(device);
        return status;
    }

    g_ViosndDevice = device;
    ViosndWriteDeviceInitDiag(physicalDeviceObject, L"Started", status);
    return status;
}
