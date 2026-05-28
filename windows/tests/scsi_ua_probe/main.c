#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SENSE_BUFFER_BYTES 32
#define DATA_BUFFER_BYTES  512
#define DEFAULT_TIMEOUT_SECONDS 10
#define DEFAULT_REPEAT_COUNT 1

#define SCSI_STATUS_GOOD 0x00
#define SCSI_STATUS_CHECK_CONDITION 0x02

typedef enum PROBE_MODE {
    PROBE_MODE_BUFFERED = 0,
    PROBE_MODE_DIRECT = 1,
} PROBE_MODE;

typedef enum PROBE_OPERATION {
    PROBE_OP_TUR = 0,
    PROBE_OP_REQUEST_SENSE = 1,
    PROBE_OP_READ_CAPACITY10 = 2,
} PROBE_OPERATION;

typedef struct PROBE_OPTIONS {
    const wchar_t* device_path;
    PROBE_MODE mode;
    PROBE_OPERATION operation;
    DWORD timeout_seconds;
    DWORD repeat_count;
} PROBE_OPTIONS;

typedef struct PROBE_RESULT {
    BOOL ioctl_ok;
    DWORD win32_error;
    DWORD bytes_returned;
    UCHAR scsi_status;
    UCHAR sense[SENSE_BUFFER_BYTES];
    ULONG sense_length;
    UCHAR data[DATA_BUFFER_BYTES];
    ULONG data_length;
    DWORD granted_access;
} PROBE_RESULT;

typedef struct SPT_WITH_BUFFERS {
    SCSI_PASS_THROUGH spt;
    ULONG filler;
    UCHAR sense[SENSE_BUFFER_BYTES];
    UCHAR data[DATA_BUFFER_BYTES];
} SPT_WITH_BUFFERS;

typedef struct SPTD_WITH_BUFFERS {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG filler;
    UCHAR sense[SENSE_BUFFER_BYTES];
    UCHAR data[DATA_BUFFER_BYTES];
} SPTD_WITH_BUFFERS;

static void print_usage(void) {
    fwprintf(
        stderr,
        L"usage: scsi_ua_probe --device <path> [--op tur|request-sense|read-capacity10] [--mode buffered|direct] [--repeat N] [--timeout N]\n"
    );
}

static const wchar_t* operation_name(PROBE_OPERATION operation) {
    switch (operation) {
    case PROBE_OP_TUR:
        return L"tur";
    case PROBE_OP_REQUEST_SENSE:
        return L"request-sense";
    case PROBE_OP_READ_CAPACITY10:
        return L"read-capacity10";
    default:
        return L"unknown";
    }
}

static const wchar_t* mode_name(PROBE_MODE mode) {
    switch (mode) {
    case PROBE_MODE_BUFFERED:
        return L"buffered";
    case PROBE_MODE_DIRECT:
        return L"direct";
    default:
        return L"unknown";
    }
}

static const wchar_t* access_name(DWORD access) {
    switch (access) {
    case GENERIC_READ | GENERIC_WRITE:
        return L"readwrite";
    case GENERIC_READ:
        return L"read";
    case 0:
        return L"query";
    default:
        return L"custom";
    }
}

static const wchar_t* scsi_status_name(UCHAR status) {
    switch (status) {
    case SCSI_STATUS_GOOD:
        return L"GOOD";
    case SCSI_STATUS_CHECK_CONDITION:
        return L"CHECK_CONDITION";
    default:
        return L"OTHER";
    }
}

static BOOL parse_u32(const wchar_t* text, DWORD* value) {
    wchar_t* end = NULL;
    unsigned long parsed = wcstoul(text, &end, 10);
    if ((text == NULL) || (value == NULL) || (text[0] == L'\0') || (end == NULL) || (*end != L'\0')) {
        return FALSE;
    }
    *value = (DWORD)parsed;
    return TRUE;
}

static BOOL parse_mode(const wchar_t* text, PROBE_MODE* mode) {
    if ((_wcsicmp(text, L"buffered") == 0) || (_wcsicmp(text, L"spt") == 0)) {
        *mode = PROBE_MODE_BUFFERED;
        return TRUE;
    }
    if ((_wcsicmp(text, L"direct") == 0) || (_wcsicmp(text, L"sptd") == 0)) {
        *mode = PROBE_MODE_DIRECT;
        return TRUE;
    }
    return FALSE;
}

static BOOL parse_operation(const wchar_t* text, PROBE_OPERATION* operation) {
    if ((_wcsicmp(text, L"tur") == 0) || (_wcsicmp(text, L"test-unit-ready") == 0)) {
        *operation = PROBE_OP_TUR;
        return TRUE;
    }
    if (_wcsicmp(text, L"request-sense") == 0) {
        *operation = PROBE_OP_REQUEST_SENSE;
        return TRUE;
    }
    if ((_wcsicmp(text, L"read-capacity10") == 0) || (_wcsicmp(text, L"read-capacity") == 0)) {
        *operation = PROBE_OP_READ_CAPACITY10;
        return TRUE;
    }
    return FALSE;
}

static BOOL parse_args(int argc, wchar_t** argv, PROBE_OPTIONS* options) {
    int index = 1;

    options->device_path = NULL;
    options->mode = PROBE_MODE_BUFFERED;
    options->operation = PROBE_OP_TUR;
    options->timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
    options->repeat_count = DEFAULT_REPEAT_COUNT;

    while (index < argc) {
        const wchar_t* arg = argv[index];
        if ((_wcsicmp(arg, L"--device") == 0) || (_wcsicmp(arg, L"-d") == 0)) {
            if ((index + 1) >= argc) {
                return FALSE;
            }
            options->device_path = argv[index + 1];
            index += 2;
            continue;
        }
        if ((_wcsicmp(arg, L"--mode") == 0) || (_wcsicmp(arg, L"-m") == 0)) {
            if (((index + 1) >= argc) || !parse_mode(argv[index + 1], &options->mode)) {
                return FALSE;
            }
            index += 2;
            continue;
        }
        if ((_wcsicmp(arg, L"--op") == 0) || (_wcsicmp(arg, L"-o") == 0)) {
            if (((index + 1) >= argc) || !parse_operation(argv[index + 1], &options->operation)) {
                return FALSE;
            }
            index += 2;
            continue;
        }
        if ((_wcsicmp(arg, L"--repeat") == 0) || (_wcsicmp(arg, L"-r") == 0)) {
            if (((index + 1) >= argc) || !parse_u32(argv[index + 1], &options->repeat_count)) {
                return FALSE;
            }
            index += 2;
            continue;
        }
        if ((_wcsicmp(arg, L"--timeout") == 0) || (_wcsicmp(arg, L"-t") == 0)) {
            if (((index + 1) >= argc) || !parse_u32(argv[index + 1], &options->timeout_seconds)) {
                return FALSE;
            }
            index += 2;
            continue;
        }
        return FALSE;
    }

    return options->device_path != NULL;
}

static void print_hex_line(const UCHAR* bytes, size_t length) {
    size_t index;
    for (index = 0; index < length; ++index) {
        wprintf(L"%02X", bytes[index]);
    }
}

static void print_sense_summary(const UCHAR* sense, ULONG sense_length) {
    if ((sense_length < 14) || (sense == NULL)) {
        wprintf(L"sense=none");
        return;
    }

    wprintf(
        L"sense=response=0x%02X,key=0x%02X,asc=0x%02X,ascq=0x%02X,bytes=",
        sense[0] & 0x7F,
        sense[2] & 0x0F,
        sense[12],
        sense[13]
    );
    print_hex_line(sense, sense_length);
}

static HANDLE open_probe_handle(const wchar_t* device_path, DWORD* granted_access) {
    static const DWORD access_candidates[] = {
        GENERIC_READ | GENERIC_WRITE,
        GENERIC_READ,
        0,
    };
    size_t index;

    for (index = 0; index < (sizeof(access_candidates) / sizeof(access_candidates[0])); ++index) {
        HANDLE handle = CreateFileW(
            device_path,
            access_candidates[index],
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        if (handle != INVALID_HANDLE_VALUE) {
            *granted_access = access_candidates[index];
            return handle;
        }
    }

    *granted_access = 0;
    return INVALID_HANDLE_VALUE;
}

static void build_cdb(PROBE_OPERATION operation, UCHAR* cdb, UCHAR* cdb_length, ULONG* data_length, UCHAR* data_in) {
    ZeroMemory(cdb, 16);
    *data_length = 0;
    *data_in = SCSI_IOCTL_DATA_UNSPECIFIED;

    switch (operation) {
    case PROBE_OP_TUR:
        cdb[0] = 0x00;
        *cdb_length = 6;
        break;
    case PROBE_OP_REQUEST_SENSE:
        cdb[0] = 0x03;
        cdb[4] = 18;
        *cdb_length = 6;
        *data_length = 18;
        *data_in = SCSI_IOCTL_DATA_IN;
        break;
    case PROBE_OP_READ_CAPACITY10:
        cdb[0] = 0x25;
        *cdb_length = 10;
        *data_length = 8;
        *data_in = SCSI_IOCTL_DATA_IN;
        break;
    default:
        *cdb_length = 6;
        break;
    }
}

static BOOL issue_buffered_probe(
    HANDLE handle,
    PROBE_OPERATION operation,
    DWORD timeout_seconds,
    PROBE_RESULT* result
) {
    SPT_WITH_BUFFERS request;
    UCHAR cdb[16];
    UCHAR cdb_length = 0;
    UCHAR data_in = SCSI_IOCTL_DATA_UNSPECIFIED;
    ULONG data_length = 0;

    ZeroMemory(&request, sizeof(request));
    build_cdb(operation, cdb, &cdb_length, &data_length, &data_in);

    request.spt.Length = (USHORT)sizeof(SCSI_PASS_THROUGH);
    request.spt.CdbLength = cdb_length;
    request.spt.SenseInfoLength = SENSE_BUFFER_BYTES;
    request.spt.DataIn = data_in;
    request.spt.DataTransferLength = data_length;
    request.spt.TimeOutValue = timeout_seconds;
    request.spt.DataBufferOffset = (data_length == 0) ? 0U : (ULONG)offsetof(SPT_WITH_BUFFERS, data);
    request.spt.SenseInfoOffset = (ULONG)offsetof(SPT_WITH_BUFFERS, sense);
    memcpy(request.spt.Cdb, cdb, cdb_length);

    result->ioctl_ok = DeviceIoControl(
        handle,
        IOCTL_SCSI_PASS_THROUGH,
        &request,
        sizeof(request),
        &request,
        sizeof(request),
        &result->bytes_returned,
        NULL
    );
    result->win32_error = result->ioctl_ok ? ERROR_SUCCESS : GetLastError();
    result->scsi_status = request.spt.ScsiStatus;
    result->sense_length = request.spt.SenseInfoLength;
    result->data_length = request.spt.DataTransferLength;
    memcpy(result->sense, request.sense, SENSE_BUFFER_BYTES);
    memcpy(result->data, request.data, DATA_BUFFER_BYTES);
    return TRUE;
}

static BOOL issue_direct_probe(
    HANDLE handle,
    PROBE_OPERATION operation,
    DWORD timeout_seconds,
    PROBE_RESULT* result
) {
    SPTD_WITH_BUFFERS request;
    UCHAR cdb[16];
    UCHAR cdb_length = 0;
    UCHAR data_in = SCSI_IOCTL_DATA_UNSPECIFIED;
    ULONG data_length = 0;

    ZeroMemory(&request, sizeof(request));
    build_cdb(operation, cdb, &cdb_length, &data_length, &data_in);

    request.sptd.Length = (USHORT)sizeof(SCSI_PASS_THROUGH_DIRECT);
    request.sptd.CdbLength = cdb_length;
    request.sptd.SenseInfoLength = SENSE_BUFFER_BYTES;
    request.sptd.DataIn = data_in;
    request.sptd.DataTransferLength = data_length;
    request.sptd.TimeOutValue = timeout_seconds;
    request.sptd.DataBuffer = (data_length == 0) ? NULL : request.data;
    request.sptd.SenseInfoOffset = (ULONG)offsetof(SPTD_WITH_BUFFERS, sense);
    memcpy(request.sptd.Cdb, cdb, cdb_length);

    result->ioctl_ok = DeviceIoControl(
        handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &request,
        sizeof(request),
        &request,
        sizeof(request),
        &result->bytes_returned,
        NULL
    );
    result->win32_error = result->ioctl_ok ? ERROR_SUCCESS : GetLastError();
    result->scsi_status = request.sptd.ScsiStatus;
    result->sense_length = request.sptd.SenseInfoLength;
    result->data_length = request.sptd.DataTransferLength;
    memcpy(result->sense, request.sense, SENSE_BUFFER_BYTES);
    memcpy(result->data, request.data, DATA_BUFFER_BYTES);
    return TRUE;
}

static void print_capacity_if_present(PROBE_OPERATION operation, const PROBE_RESULT* result) {
    if ((operation == PROBE_OP_READ_CAPACITY10) && (result->data_length >= 8)) {
        uint32_t last_lba = ((uint32_t)result->data[0] << 24) |
            ((uint32_t)result->data[1] << 16) |
            ((uint32_t)result->data[2] << 8) |
            ((uint32_t)result->data[3]);
        uint32_t block_size = ((uint32_t)result->data[4] << 24) |
            ((uint32_t)result->data[5] << 16) |
            ((uint32_t)result->data[6] << 8) |
            ((uint32_t)result->data[7]);
        unsigned long long total_bytes = ((unsigned long long)last_lba + 1ULL) * block_size;
        wprintf(L",capacity=last_lba=%lu,block_size=%lu,total_bytes=%llu", last_lba, block_size, total_bytes);
    }
}

static void print_request_sense_if_present(PROBE_OPERATION operation, const PROBE_RESULT* result) {
    if ((operation == PROBE_OP_REQUEST_SENSE) && (result->data_length != 0)) {
        wprintf(L",sense-data=");
        print_hex_line(result->data, result->data_length);
    }
}

int wmain(int argc, wchar_t** argv) {
    PROBE_OPTIONS options;
    DWORD access = 0;
    HANDLE handle;
    DWORD index;

    if (!parse_args(argc, argv, &options)) {
        print_usage();
        return 2;
    }

    handle = open_probe_handle(options.device_path, &access);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        fwprintf(stderr, L"open-failed device=%ls last_error=%lu\n", options.device_path, error);
        return 1;
    }

    for (index = 0; index < options.repeat_count; ++index) {
        PROBE_RESULT result;
        ZeroMemory(&result, sizeof(result));
        result.granted_access = access;

        if (options.mode == PROBE_MODE_BUFFERED) {
            issue_buffered_probe(handle, options.operation, options.timeout_seconds, &result);
        } else {
            issue_direct_probe(handle, options.operation, options.timeout_seconds, &result);
        }

        wprintf(
            L"probe=%lu,device=%ls,mode=%ls,op=%ls,access=%ls,ioctl_ok=%ls,last_error=%lu,scsi_status=0x%02X(%ls),bytes_returned=%lu,",
            index + 1,
            options.device_path,
            mode_name(options.mode),
            operation_name(options.operation),
            access_name(result.granted_access),
            result.ioctl_ok ? L"true" : L"false",
            result.win32_error,
            result.scsi_status,
            scsi_status_name(result.scsi_status),
            result.bytes_returned
        );
        print_sense_summary(result.sense, result.sense_length);
        print_request_sense_if_present(options.operation, &result);
        print_capacity_if_present(options.operation, &result);
        wprintf(L"\n");
    }

    CloseHandle(handle);
    return 0;
}
