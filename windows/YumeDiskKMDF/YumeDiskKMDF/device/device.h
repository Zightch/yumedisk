#pragma once

#include "..\core\defs.h"

NTSTATUS
ControlAddDevice(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
);

