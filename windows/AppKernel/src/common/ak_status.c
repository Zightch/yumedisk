#include "common/ak_internal.h"

AK_STATUS AkFromWin32Error(
    DWORD error)
{
    switch (error) {
    case ERROR_SUCCESS:
        return AK_STATUS_SUCCESS;

    case ERROR_INVALID_PARAMETER:
        return AK_STATUS_INVALID_PARAMETER;

    case ERROR_OUTOFMEMORY:
    case ERROR_NOT_ENOUGH_MEMORY:
        return AK_STATUS_INSUFFICIENT_RESOURCES;

    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NOT_FOUND:
    case ERROR_NO_MORE_ITEMS:
        return AK_STATUS_NOT_FOUND;

    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return AK_STATUS_NOT_SUPPORTED;

    case ERROR_NOT_READY:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_GEN_FAILURE:
    case ERROR_IO_DEVICE:
    case ERROR_DEVICE_HARDWARE_ERROR:
        return AK_STATUS_DEVICE_NOT_READY;

    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT:
        return AK_STATUS_TIMEOUT;

    case ERROR_CANCELLED:
    case ERROR_OPERATION_ABORTED:
        return AK_STATUS_CANCELLED;

    default:
        return AK_STATUS_UNSUCCESSFUL;
    }
}
