/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Smartcard Device Service Virtual Channel
 *
 * Copyright 2011 O.S. Systems Software Ltda.
 * Copyright 2011 Eduardo Fiss Beloni <beloni@ossystems.com.br>
 * Copyright 2011 Anthony Tong <atong@trustedcs.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/smartcard.h>
#include <winpr/environment.h>

#include <freerdp/channels/rdpdr.h>

#include "smartcard_main.h"

static BOOL g_SmartCardAsync = TRUE;

static void smartcard_free(DEVICE* device)
{
	SMARTCARD_DEVICE* smartcard = (SMARTCARD_DEVICE*) device;

	MessageQueue_PostQuit(smartcard->IrpQueue, 0);
	WaitForSingleObject(smartcard->thread, INFINITE);

	CloseHandle(smartcard->thread);

	Stream_Free(smartcard->device.data, TRUE);

	MessageQueue_Free(smartcard->IrpQueue);
	ListDictionary_Free(smartcard->rgSCardContextList);
	ListDictionary_Free(smartcard->rgOutstandingMessages);

	free(device);
}

/**
 * Initialization occurs when the protocol server sends a device announce message.
 * At that time, we need to cancel all outstanding IRPs.
 */

static void smartcard_init(DEVICE* device)
{
	int index;
	int keyCount;
	ULONG_PTR* pKeys;
	SCARDCONTEXT hContext;

	SMARTCARD_DEVICE* smartcard = (SMARTCARD_DEVICE*) device;

	/**
	 * On protocol termination, the following actions are performed:
	 * For each context in rgSCardContextList, SCardCancel is called causing all outstanding messages to be processed.
	 * After there are no more outstanding messages, SCardReleaseContext is called on each context and the context MUST
	 * be removed from rgSCardContextList.
	 */

	if (ListDictionary_Count(smartcard->rgSCardContextList) > 0)
	{
		pKeys = NULL;
		keyCount = ListDictionary_GetKeys(smartcard->rgSCardContextList, &pKeys);

		for (index = 0; index < keyCount; index++)
		{
			hContext = (SCARDCONTEXT) ListDictionary_GetItemValue(smartcard->rgSCardContextList, (void*) pKeys[index]);

			SCardCancel(hContext);
		}

		free(pKeys);
	}
}

void smartcard_complete_irp(SMARTCARD_DEVICE* smartcard, IRP* irp)
{
	void* key;

	key = (void*) (size_t) irp->CompletionId;
	ListDictionary_Remove(smartcard->rgOutstandingMessages, key);

	irp->Complete(irp);
}

void* smartcard_process_irp_worker_proc(IRP* irp)
{
	SMARTCARD_DEVICE* smartcard;

	smartcard = (SMARTCARD_DEVICE*) irp->device;

	smartcard_irp_device_control(smartcard, irp);

	ExitThread(0);
	return NULL;
}

/**
 * Multiple threads and SCardGetStatusChange:
 * http://musclecard.996296.n3.nabble.com/Multiple-threads-and-SCardGetStatusChange-td4430.html
 */

void smartcard_process_irp(SMARTCARD_DEVICE* smartcard, IRP* irp)
{
	void* key;
	BOOL asyncIrp = FALSE;
	UINT32 ioControlCode = 0;

	key = (void*) (size_t) irp->CompletionId;
	ListDictionary_Add(smartcard->rgOutstandingMessages, key, irp);

	if (irp->MajorFunction == IRP_MJ_DEVICE_CONTROL)
	{
		smartcard_irp_device_control_peek_io_control_code(smartcard, irp, &ioControlCode);

		if (!ioControlCode)
			return;

		if (g_SmartCardAsync)
		{
			asyncIrp = TRUE;

			switch (ioControlCode)
			{
				case SCARD_IOCTL_ESTABLISHCONTEXT:
				case SCARD_IOCTL_RELEASECONTEXT:
				case SCARD_IOCTL_ISVALIDCONTEXT:
				case SCARD_IOCTL_ACCESSSTARTEDEVENT:
				case SCARD_IOCTL_RELEASESTARTEDEVENT:
					asyncIrp = FALSE;
					break;

				case SCARD_IOCTL_TRANSMIT:
				case SCARD_IOCTL_STATUSA:
				case SCARD_IOCTL_STATUSW:
				case SCARD_IOCTL_GETSTATUSCHANGEA:
				case SCARD_IOCTL_GETSTATUSCHANGEW:
					asyncIrp = TRUE;
					break;
			}
		}

		if (!asyncIrp)
		{
			smartcard_irp_device_control(smartcard, irp);
		}
		else
		{
			irp->thread = CreateThread(NULL, 0,
					(LPTHREAD_START_ROUTINE) smartcard_process_irp_worker_proc,
					irp, 0, NULL);
		}
	}
	else
	{
		fprintf(stderr, "Unexpected SmartCard IRP: MajorFunction 0x%08X MinorFunction: 0x%08X",
				irp->MajorFunction, irp->MinorFunction);
		irp->IoStatus = STATUS_NOT_SUPPORTED;
		smartcard_complete_irp(smartcard, irp);
	}
}

static void* smartcard_thread_func(void* arg)
{
	IRP* irp;
	wMessage message;
	SMARTCARD_DEVICE* smartcard = (SMARTCARD_DEVICE*) arg;

	while (1)
	{
		if (!MessageQueue_Wait(smartcard->IrpQueue))
			break;

		if (!MessageQueue_Peek(smartcard->IrpQueue, &message, TRUE))
			break;

		if (message.id == WMQ_QUIT)
			break;

		irp = (IRP*) message.wParam;

		if (irp)
			smartcard_process_irp(smartcard, irp);
	}

	ExitThread(0);
	return NULL;
}

static void smartcard_irp_request(DEVICE* device, IRP* irp)
{
	SMARTCARD_DEVICE* smartcard = (SMARTCARD_DEVICE*) device;
	MessageQueue_Post(smartcard->IrpQueue, NULL, 0, (void*) irp, NULL);
}

/* smartcard is always built-in */
#define DeviceServiceEntry	smartcard_DeviceServiceEntry

int DeviceServiceEntry(PDEVICE_SERVICE_ENTRY_POINTS pEntryPoints)
{
	char* name;
	char* path;
	int length, ck;
	RDPDR_SMARTCARD* device;
	SMARTCARD_DEVICE* smartcard;

	device = (RDPDR_SMARTCARD*) pEntryPoints->device;

	name = device->Name;
	path = device->Path;

	smartcard = (SMARTCARD_DEVICE*) calloc(1, sizeof(SMARTCARD_DEVICE));

	if (!smartcard)
		return -1;

	smartcard->device.type = RDPDR_DTYP_SMARTCARD;
	smartcard->device.name = "SCARD";
	smartcard->device.IRPRequest = smartcard_irp_request;
	smartcard->device.Init = smartcard_init;
	smartcard->device.Free = smartcard_free;

	length = strlen(smartcard->device.name);
	smartcard->device.data = Stream_New(NULL, length + 1);

	Stream_Write(smartcard->device.data, "SCARD", 6);

	smartcard->name = NULL;
	smartcard->path = NULL;

	if (path)
	{
		smartcard->path = path;
		smartcard->name = name;
	}
	else if (name)
	{
		if (1 == sscanf(name, "%d", &ck))
			smartcard->path = name;
		else
			smartcard->name = name;
	}

	smartcard->log = WLog_Get("com.freerdp.channel.smartcard.client");

	WLog_SetLogLevel(smartcard->log, WLOG_DEBUG);

	smartcard->IrpQueue = MessageQueue_New(NULL);
	smartcard->rgSCardContextList = ListDictionary_New(TRUE);
	smartcard->rgOutstandingMessages = ListDictionary_New(TRUE);

	smartcard->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) smartcard_thread_func,
			smartcard, CREATE_SUSPENDED, NULL);

	pEntryPoints->RegisterDevice(pEntryPoints->devman, (DEVICE*) smartcard);

	ResumeThread(smartcard->thread);

	return 0;
}

