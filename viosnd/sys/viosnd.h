#pragma once

#define VIOSND_POOL_TAG 'dnSV'
/*
 * What PcAddAdapterDevice is told this adapter will ever register, and a hard budget: the call
 * past it fails with STATUS_ALLOTTED_SPACE_EXCEEDED, which reads like a resource problem rather
 * than like a number this driver chose.
 *
 * Every endpoint costs two -- a topology subdevice and a wave subdevice -- so this is four
 * endpoints in each direction. Adapter.cpp asserts that against VIOSND_MAX_ENDPOINTS, which is
 * where the real limit lives; the two cannot be kept in one place because this header is needed
 * before the endpoint code is.
 */
#define VIOSND_MAX_SUBDEVICES 16

extern "C" DRIVER_ADD_DEVICE XcbVirtioAudioAddDevice;

extern "C" NTSTATUS
XcbVirtioAudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList);

/* Records a line of driver state under the device's registry key; see the definition. */
VOID
ViosndWriteDeviceDiagString(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_z_ PCWSTR ValueName,
    _In_z_ PCWSTR Value);
