#pragma once

#include "..\core\defs.h"

EVT_WDF_DEVICE_FILE_CREATE ControlEvtFileCreate;
EVT_WDF_FILE_CLEANUP ControlEvtFileCleanup;
EVT_WDF_FILE_CLOSE ControlEvtFileClose;

