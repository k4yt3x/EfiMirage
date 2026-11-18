#include "EfiMirageDxe.h"

#include <Protocol/Shell.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/SynchronizationLib.h>

//
// EFI Driver Version Protocol
//
EFI_DRIVER_SUPPORTED_EFI_VERSION_PROTOCOL gEfiMirageSupportedEfiVersion =
{
	sizeof(EFI_DRIVER_SUPPORTED_EFI_VERSION_PROTOCOL),
	EFI_2_10_SYSTEM_TABLE_REVISION
};

//
// Driver unload
//
EFI_STATUS
EFIAPI
EfiMirageUnload(
	IN EFI_HANDLE ImageHandle
	);

//
// EfiMirage driver protocol
//
EFI_STATUS
EFIAPI
DriverConfigure(
	IN CONST EFIMIRAGE_CONFIGURATION_DATA* ConfigurationData
	);

EFIMIRAGE_DRIVER_PROTOCOL gEfiMirageDriverProtocol =
{
	DriverConfigure
};

//
// GUID of the boot-service state variable the Windows-side client reads.
//
STATIC CONST EFI_GUID mStateVariableGuid = EFIMIRAGE_STATE_VARIABLE_GUID;

//
// Records what this boot is emulating so the Windows-side client can tell an emulated VBS state
// from a real one. Boot service + runtime only, which makes it volatile: it disappears at power
// loss and is never written to NVRAM.
//
STATIC
VOID
EFIAPI
UpdateStateVariable(
	VOID
	)
{
	EFIMIRAGE_STATE_DATA State;
	State.Magic = EFIMIRAGE_STATE_MAGIC;
	State.Version = EFIMIRAGE_STATE_VERSION;
	State.Flags = 0;
	if (gDriverConfig.FakeVbs != 0)
		State.Flags |= EFIMIRAGE_STATE_FLAG_FAKE_VBS;
	if (gDriverConfig.FakeSecureBoot)
		State.Flags |= EFIMIRAGE_STATE_FLAG_FAKE_SECURE_BOOT;

	if (State.Flags == 0)
	{
		gRT->SetVariable((CHAR16*)EFIMIRAGE_STATE_VARIABLE_NAME, (EFI_GUID*)&mStateVariableGuid, 0, 0, NULL);
		return;
	}

	CONST EFI_STATUS Status = gRT->SetVariable((CHAR16*)EFIMIRAGE_STATE_VARIABLE_NAME,
												(EFI_GUID*)&mStateVariableGuid,
												EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
												sizeof(State),
												&State);
	if (EFI_ERROR(Status))
		Print(L"WARNING: failed to record emulation state: %llx (%r)\r\n", Status, Status);
}

//
// Default driver configuration used if Configure() is not called
//
EFIMIRAGE_CONFIGURATION_DATA gDriverConfig = {
	EFIMIRAGE_CONFIGURATION_DATA_VERSION,	// Version
	DSE_DISABLE_SETVARIABLE_HOOK,			// DseBypassMethod
	FALSE,									// WaitForKeyPress
	TRUE,									// FakeSecureBoot
	0,										// FakeVbs
	{ 0, 0, 0 }								// Reserved
};

//
// Bootmgfw.efi handle
//
EFI_HANDLE gBootmgfwHandle = NULL;

//
// EFI runtime globals
//
EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* gTextInputEx = NULL;
EFI_EVENT gEfiExitBootServicesEvent = NULL;
BOOLEAN gEfiAtRuntime = FALSE;
EFI_EVENT gEfiVirtualNotifyEvent = NULL;
BOOLEAN gEfiGoneVirtual = FALSE;

//
// Original boot-service image entry points
//
STATIC EFI_IMAGE_LOAD mOriginalLoadImage = NULL;
STATIC EFI_IMAGE_START mOriginalStartImage = NULL;

// StartImage scopes may nest. Only the innermost image identifies the current boot attempt.
STATIC BOOLEAN mWindowsBootActive = FALSE;
STATIC UINTN mStartImageDepth = 0;

//
// Original gRT->SetVariable pointer
//
STATIC EFI_SET_VARIABLE mOriginalSetVariable = NULL;

#if defined(MDE_CPU_X64)
#define MM_SYSTEM_RANGE_START	(VOID*)(0xFFFF080000000000) // Windows XP through 7 value. On newer systems this is a bit higher, but not that much
#elif defined(MDE_CPU_IA32)
#define MM_SYSTEM_RANGE_START	(VOID*)(0x80000000)
#endif

#define EFIMIRAGE_TITLE		L"\r\n EfiMirage\r\n" \
							L" UEFI boot-chain patching and Secure Boot state emulation\r\n"


//
// (Un)hooks a service table pointer, replacing its value with NewFunction and returning the original address.
//
VOID*
SetServicePointer(
	IN OUT EFI_TABLE_HEADER *ServiceTableHeader,
	IN OUT VOID **ServiceTableFunction,
	IN VOID *NewFunction
	)
{
	if (ServiceTableFunction == NULL || NewFunction == NULL)
		return NULL;

	// If this is really needed after boot time at some point the CRC function is easy enough to reimplement
	ASSERT(gBS != NULL);
	ASSERT(gBS->CalculateCrc32 != NULL);

	CONST EFI_TPL Tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL); // Note: implies cli
	CONST UINTN Cr0 = AsmReadCr0();
	CONST BOOLEAN WpSet = (Cr0 & CR0_WP) != 0;
	if (WpSet)
		AsmWriteCr0(Cr0 & ~CR0_WP);

	VOID* OriginalFunction = InterlockedCompareExchangePointer(ServiceTableFunction,
																*ServiceTableFunction,
																NewFunction);

	// Recalculate the table checksum
	ServiceTableHeader->CRC32 = 0;
	gBS->CalculateCrc32((UINT8*)ServiceTableHeader, ServiceTableHeader->HeaderSize, &ServiceTableHeader->CRC32);

	if (WpSet)
		AsmWriteCr0(Cr0);
	gBS->RestoreTPL(Tpl);

	return OriginalFunction;
}

//
// Boot Services LoadImage hook
//
EFI_STATUS
EFIAPI
HookedLoadImage(
	IN BOOLEAN BootPolicy,
	IN EFI_HANDLE ParentImageHandle,
	IN EFI_DEVICE_PATH_PROTOCOL *DevicePath,
	IN VOID *SourceBuffer OPTIONAL,
	IN UINTN SourceSize,
	OUT EFI_HANDLE *ImageHandle
	)
{
	// Try to get a readable file path from the EFI shell protocol if it's available
	EFI_SHELL_PROTOCOL* EfiShellProtocol = NULL;
	CONST EFI_STATUS EfiShellStatus = gBS->LocateProtocol(&gEfiShellProtocolGuid,
															NULL,
															(VOID**)&EfiShellProtocol);
	CHAR16* ImagePath = NULL;
	if (!EFI_ERROR(EfiShellStatus))
	{
		ImagePath = EfiShellProtocol->GetFilePathFromDevicePath(DevicePath);
	}
	if (ImagePath == NULL)
	{
		ImagePath = ConvertDevicePathToText(DevicePath, TRUE, TRUE);
	}

	// We only have a filename to go on at this point. We will determine the final 'is this bootmgfw.efi?' status after the image has been loaded
	CONST BOOLEAN MaybeBootmgfw = ImagePath != NULL
		? StriStr(ImagePath, L"bootmgfw.efi") != NULL || StriStr(ImagePath, L"Bootmgfw_ms.vc") != NULL || StriStr(ImagePath, L"bootx64.efi") != NULL
		: FALSE;
	CONST BOOLEAN IsBoot = (MaybeBootmgfw || (BootPolicy == TRUE && SourceBuffer == NULL));

	// Print what's being loaded or booted
	CONST INT32 OriginalAttribute = SetConsoleTextColour(EFI_GREEN, FALSE);
	Print(L"[HookedLoadImage] %S %S\r\n    (ParentImageHandle = %llx)\r\n",
		(IsBoot ? L"Booting" : L"Loading"), ImagePath, (UINTN)ParentImageHandle);
	if (ImagePath != NULL)
		FreePool(ImagePath);
	RtlSleep(500);

	// Q: If we loaded bootmgfw.efi manually, is there any benefit to flipping BootPolicy to TRUE
	// to make it look like the load request came straight from the boot manager?
	if (MaybeBootmgfw)
	{
		// Let's find out
		BootPolicy = TRUE;
	}

	// Load the image
	CONST EFI_STATUS Status = mOriginalLoadImage(BootPolicy,
												ParentImageHandle,
												DevicePath,
												SourceBuffer,
												SourceSize,
												ImageHandle);

	// Was this a successful load of an image that's being booted?
	if (!EFI_ERROR(Status) && IsBoot && *ImageHandle != NULL)
	{
		// Get loaded image info
		EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
		CONST EFI_STATUS ImageInfoStatus = gBS->OpenProtocol(*ImageHandle,
															&gEfiLoadedImageProtocolGuid,
															(VOID**)&LoadedImage,
															gImageHandle,
															NULL,
															EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (EFI_ERROR(ImageInfoStatus))
		{
			Print(L"\r\nHookedLoadImage: failed to get loaded image info. Status: %llx (%r)\r\n",
				ImageInfoStatus, ImageInfoStatus);
		}
		else
		{
			// Determine the type of file we're loading
			CONST INPUT_FILETYPE FileType = GetInputFileType(LoadedImage->ImageBase, LoadedImage->ImageSize);
			ASSERT(FileType == Unknown || FileType == Bootmgr || FileType == BootmgfwEfi);

			if (FileType == BootmgfwEfi)
			{
				// This is bootmgfw.efi. Save the returned image handle
				gBootmgfwHandle = *ImageHandle;
				LoadedImage->ParentHandle = NULL;

				// Print image info
				PrintLoadedImageInfo(LoadedImage);

				// Nuke it dot it
				PatchBootManager(FileType,
								LoadedImage->ImageBase,
								LoadedImage->ImageSize);
			}
		}
	}

	gST->ConOut->SetAttribute(gST->ConOut, OriginalAttribute);
	gST->ConOut->EnableCursor(gST->ConOut, FALSE);

	return Status;
}

//
// Loaded Windows images may return without booting, or start a different OS as a child.
// Inspect the image being started rather than a saved handle, which firmware may reuse.
//
EFI_STATUS
EFIAPI
HookedStartImage(
	IN EFI_HANDLE ImageHandle,
	OUT UINTN* ExitDataSize,
	OUT CHAR16** ExitData OPTIONAL
	)
{
	CONST BOOLEAN PreviousWindowsBoot = mWindowsBootActive;
	mWindowsBootActive = FALSE;
	++mStartImageDepth;

	EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
	if (!EFI_ERROR(gBS->HandleProtocol(ImageHandle,
									&gEfiLoadedImageProtocolGuid,
									(VOID**)&LoadedImage)) && LoadedImage != NULL)
	{
		// Any image of the Windows boot chain counts, not only bootmgfw.efi: a WIM boot goes
		// bootmgfw.efi -> bootmgr.efi -> winload.efi, and each of those may or may not be entered
		// through gBS->StartImage depending on the firmware and the Windows version.
		CONST INPUT_FILETYPE FileType = GetInputFileType(LoadedImage->ImageBase, LoadedImage->ImageSize);
		mWindowsBootActive = FileType == BootmgfwEfi || FileType == BootmgrEfi || FileType == WinloadEfi;
	}

	CONST EFI_STATUS Status = mOriginalStartImage(ImageHandle, ExitDataSize, ExitData);
	mWindowsBootActive = PreviousWindowsBoot;
	--mStartImageDepth;
	return Status;
}

STATIC
BOOLEAN
EFIAPI
IsSetVariableHookRequired(
	VOID
	)
{
	return gDriverConfig.DseBypassMethod == DSE_DISABLE_SETVARIABLE_HOOK &&
		gBootmgfwHandle != NULL && mWindowsBootActive;
}

//
// Runtime Services SetVariable hook
//
EFI_STATUS
EFIAPI
HookedSetVariable(
	IN CHAR16 *VariableName,
	IN EFI_GUID *VendorGuid,
	IN UINT32 Attributes,
	IN UINTN DataSize,
	IN VOID *Data
	)
{
	// Eligibility is frozen by the active StartImage scope when boot services end.
	ASSERT(!gEfiAtRuntime || IsSetVariableHookRequired());

	if (gEfiAtRuntime && gEfiGoneVirtual && IsSetVariableHookRequired() &&
		VariableName != NULL && VariableName[0] != CHAR_NULL && VendorGuid != NULL &&
		CompareGuid(VendorGuid, EFIMIRAGE_BACKDOOR_VARIABLE_GUID) &&
		StrnCmp(VariableName, EFIMIRAGE_BACKDOOR_VARIABLE_NAME, (sizeof(EFIMIRAGE_BACKDOOR_VARIABLE_NAME) / sizeof(CHAR16)) - 1) == 0)
	{
		// Yep. Do we have any data?
		if (DataSize == 0 && Data == NULL)
		{
			// Nope. This is the first SetVariable() call from the HAL, intended to wipe the variable.
			// (This call may be skipped if EFI_VARIABLE_APPEND_WRITE is set, but this is version-dependent)
			return EFI_SUCCESS;
		}

		if ((Attributes & EFIMIRAGE_BACKDOOR_VARIABLE_ATTRIBUTES) == EFIMIRAGE_BACKDOOR_VARIABLE_ATTRIBUTES &&
			DataSize == EFIMIRAGE_BACKDOOR_VARIABLE_DATASIZE &&
			Data != NULL)
		{
			// Yep, and Attributes and DataSize are correct. Check if *Data is a valid input for a backdoor read/write operation
			EFIMIRAGE_BACKDOOR_DATA* BackdoorData = Data;
			if (BackdoorData->CookieValue == EFIMIRAGE_BACKDOOR_COOKIE_VALUE &&
				BackdoorData->Size > 0 &&
				(UINTN)BackdoorData->KernelAddress >= (UINTN)MM_SYSTEM_RANGE_START)
			{
				// For scalars, copy user value to kernel memory and put the old value in BackdoorData->u.XXX
				switch (BackdoorData->Size)
				{
					case 1:
					{
						CONST UINT8 NewByte = (UINT8)BackdoorData->u.s.Byte;
						BackdoorData->u.s.Byte = *(UINT8*)BackdoorData->KernelAddress;
						if (!BackdoorData->ReadOnly)
							CopyWpMem(BackdoorData->KernelAddress, &NewByte, sizeof(NewByte));
						break;
					}
					case 2:
					{
						CONST UINT16 NewWord = (UINT16)BackdoorData->u.s.Word;
						BackdoorData->u.s.Word = *(UINT16*)BackdoorData->KernelAddress;
						if (!BackdoorData->ReadOnly)
							CopyWpMem(BackdoorData->KernelAddress, &NewWord, sizeof(NewWord));
						break;
					}
					case 4:
					{
						CONST UINT32 NewDword = (UINT32)BackdoorData->u.s.Dword;
						BackdoorData->u.s.Dword = *(UINT32*)BackdoorData->KernelAddress;
						if (!BackdoorData->ReadOnly)
							CopyWpMem(BackdoorData->KernelAddress, &NewDword, sizeof(NewDword));
						break;
					}
					case 8:
					{
						CONST UINT64 NewQword = BackdoorData->u.Qword;
						BackdoorData->u.Qword = *(UINT64*)BackdoorData->KernelAddress;
						if (!BackdoorData->ReadOnly)
							CopyWpMem(BackdoorData->KernelAddress, &NewQword, sizeof(NewQword));
						break;
					}
					default:
					{
						// Arbitrary size memcpy
						if (BackdoorData->u.UserBuffer != NULL)
						{
							if (BackdoorData->ReadOnly)
								CopyWpMem(BackdoorData->u.UserBuffer, BackdoorData->KernelAddress, BackdoorData->Size);
							else
								CopyWpMem(BackdoorData->KernelAddress, BackdoorData->u.UserBuffer, BackdoorData->Size);
						}
						break;
					}
				}

				// Backdoor complete
				return EFI_SUCCESS;
			}
			//else { /*Invalid EFIMIRAGE_BACKDOOR_DATA* provided*/ }
		}
		//else { /*Data is NULL, or DataSize/Attributes mismatch*/ }
	}
	//else { /*Not our variable name + vendor GUID, or SetVirtualAddressMap() has not been called yet*/ }

	return mOriginalSetVariable(VariableName, VendorGuid, Attributes, DataSize, Data);
}

//
// ExitBootServices callback
//
VOID
EFIAPI
ExitBootServicesEvent(
	IN EFI_EVENT Event,
	IN VOID* Context
	)
{
	// Close this event now. The boot loader only calls this once.
	gBS->CloseEvent(gEfiExitBootServicesEvent);
	gEfiExitBootServicesEvent = NULL;

	// The message buffer may be empty if the patch process was aborted in one of the earlier stages
	if (gKernelPatchInfo.Buffer[0] != CHAR_NULL)
	{
		CONST EFI_STATUS Status = gKernelPatchInfo.Status;
		CONST INT32 OriginalAttribute = gST->ConOut->Mode->Attribute;

		// Default to showing a message in case of errors unless we are booting a pre-Vista kernel such as XP, in which case EFI_UNSUPPORTED is expected.
		CONST BOOLEAN ShowErrorMessage = gKernelPatchInfo.KernelBuildNumber == 0 || gKernelPatchInfo.KernelBuildNumber >= 6001 || Status != EFI_UNSUPPORTED;
		if (Status == EFI_SUCCESS)
		{
			SetConsoleTextColour(EFI_GREEN, TRUE);
			PrintKernelPatchInfo();
			Print(L"\r\nSuccessfully patched ntoskrnl.exe.\r\n");

			if (gDriverConfig.WaitForKeyPress)
			{
				Print(L"\r\nPress any key to continue.\r\n");
				WaitForKey();
			}
		}
		else if (ShowErrorMessage)
		{
			// Patch failed. Most important stuff first: make a fake BSOD, because... reasons
			// TODO if really bored: use GOP to set the BG colour on the whole screen.
			// Could add one of those obnoxious Win 10 :( smileys and a QR code
			gST->ConOut->SetAttribute(gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLUE);
			gST->ConOut->ClearScreen(gST->ConOut);

			Print(L"A problem has been detected and Windows has been paused to prevent damage\r\nto your botnets.\r\n\r\n"
				L"BOOTKIT_KERNEL_PATCH_FAILED\r\n\r\n"
				L"Technical information:\r\n\r\n*** STOP: 0X%llX (%r, 0x%p)\r\n\r\n",
				Status, Status, gKernelPatchInfo.KernelBase);
			PrintKernelPatchInfo();

			// Give time for user to register their loss and allow for the grieving process to set in
			RtlStall(2000);

			// Prompt user to ask what they want to do
			Print(L"\r\nPress any key to continue anyway, or press ESC to reboot.\r\n");
			if (!WaitForKey())
			{
				gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
			}
		}

		gST->ConOut->SetAttribute(gST->ConOut, OriginalAttribute);
		if (Status != EFI_SUCCESS && ShowErrorMessage)
			gST->ConOut->ClearScreen(gST->ConOut);
	}

	// A loaded but returned Windows image must not keep the hook for a fallback OS.
	CONST BOOLEAN KeepSetVariableHook = IsSetVariableHookRequired();
	if (!KeepSetVariableHook && mOriginalSetVariable != NULL)
	{
		SetServicePointer(&gRT->Hdr, (VOID**)&gRT->SetVariable, (VOID*)mOriginalSetVariable);
		mOriginalSetVariable = NULL;
	}

	// Same symmetry for GetVariable: it is only worth keeping while emulation is on.
	if (!gDriverConfig.FakeSecureBoot)
		UninstallSecureBootHook();

	// Tear down everything else only when no runtime hook is left to serve. Otherwise the driver must stay
	// linked as /SUBSYSTEM:EFI_RUNTIME_DRIVER and its image mapped for the lifetime of the call.
	if (!KeepSetVariableHook && !gDriverConfig.FakeSecureBoot)
	{
		// Uninstall our installed driver protocols
		gBS->UninstallMultipleProtocolInterfaces(gImageHandle,
												&gEfiMirageDriverProtocolGuid,
												&gEfiMirageDriverProtocol,
												&gEfiDriverSupportedEfiVersionProtocolGuid,
												&gEfiMirageSupportedEfiVersion,
												NULL);

		// Unregister SetVirtualAddressMap() notification
		if (gEfiVirtualNotifyEvent != NULL)
		{
			gBS->CloseEvent(gEfiVirtualNotifyEvent);
			gEfiVirtualNotifyEvent = NULL;
		}
	}

	// Regardless of which OS is being booted, boot services won't be available after this callback returns
	gBS = NULL;
	mOriginalLoadImage = NULL;
	mOriginalStartImage = NULL;
	gEfiAtRuntime = TRUE;
}

//
// SetVirtualAddressMap callback
//
VOID
EFIAPI
SetVirtualAddressMapEvent(
	IN EFI_EVENT Event,
	IN VOID* Context
	)
{
	ASSERT(gEfiAtRuntime == TRUE);
	ASSERT(gBS == NULL);
	gEfiVirtualNotifyEvent = NULL;

	// Convert the original SetVariable pointer to virtual so our hook will continue to work.
	// It is only installed when the backdoor is in use; Secure Boot emulation may keep the
	// notification alive without it.
	EFI_STATUS Status = EFI_SUCCESS;
	if (mOriginalSetVariable != NULL)
	{
		Status = gRT->ConvertPointer(0, (VOID**)&mOriginalSetVariable);
		ASSERT_EFI_ERROR(Status);
	}

	// Same for the original GetVariable pointer if the Secure Boot emulation hook is still installed
	SecureBootConvertRuntimePointers();

	// Convert the runtime services pointer itself from physical to virtual
	Status = gRT->ConvertPointer(0, (VOID**)&gRT);
	ASSERT_EFI_ERROR(Status);

	// Set the flag indicating virtual addressing mode has been entered
	gEfiGoneVirtual = TRUE;
}

EFI_STATUS
EFIAPI
DriverConfigure(
	IN CONST EFIMIRAGE_CONFIGURATION_DATA* ConfigurationData
	)
{
	// Do not allow configure if we are at runtime, or if the Windows boot manager has been loaded
	if (gEfiAtRuntime || gBootmgfwHandle != NULL)
		return EFI_ACCESS_DENIED;

	if (ConfigurationData == NULL)
		return EFI_INVALID_PARAMETER;

	if (ConfigurationData->Version != EFIMIRAGE_CONFIGURATION_DATA_VERSION)
	{
		Print(L"Configuration data version mismatch: got %u, expected %u.\r\n",
			(UINTN)ConfigurationData->Version, (UINTN)EFIMIRAGE_CONFIGURATION_DATA_VERSION);
		return EFI_INCOMPATIBLE_VERSION;
	}

	// Validate before committing anything: an out-of-range method would silently fall through to
	// DSE_DISABLE_NONE in the patch stages. Compare unsigned so a negative enum value (whose
	// underlying type is signed under MSVC) is rejected rather than passing the range check.
	if ((UINT32)ConfigurationData->DseBypassMethod > (UINT32)DSE_DISABLE_SETVARIABLE_HOOK)
		return EFI_INVALID_PARAMETER;

	if (ConfigurationData->FakeVbs > 2)
		return EFI_INVALID_PARAMETER;

	gDriverConfig = *ConfigurationData;

	Print(L"Configuration data accepted: DSE bypass %u, wait for keypress %u, fake Secure Boot %u, fake VBS %u.\r\n\r\n",
		(UINTN)gDriverConfig.DseBypassMethod,
		(UINTN)gDriverConfig.WaitForKeyPress,
		(UINTN)gDriverConfig.FakeSecureBoot,
		(UINTN)gDriverConfig.FakeVbs);

	UpdateStateVariable();

	return EFI_SUCCESS;
}

//
// Driver unload
//
EFI_STATUS
EFIAPI
EfiMirageUnload(
	IN EFI_HANDLE ImageHandle
	)
{
	// Patched images and in-flight StartImage wrappers may still refer to this driver.
	if (gEfiAtRuntime || gBootmgfwHandle != NULL || mStartImageDepth != 0)
	{
		return EFI_ACCESS_DENIED;
	}

	ASSERT(gBS != NULL);

	// Uninstall our installed driver protocols
	gBS->UninstallMultipleProtocolInterfaces(gImageHandle,
											&gEfiMirageDriverProtocolGuid,
											&gEfiMirageDriverProtocol,
											&gEfiDriverSupportedEfiVersionProtocolGuid,
											&gEfiMirageSupportedEfiVersion,
											NULL);

	// Unregister SetVirtualAddressMap() notification
	if (gEfiVirtualNotifyEvent != NULL)
	{
		gBS->CloseEvent(gEfiVirtualNotifyEvent);
		gEfiVirtualNotifyEvent = NULL;
	}

	// Unregister ExitBootServices() notification
	if (gEfiExitBootServicesEvent != NULL)
	{
		gBS->CloseEvent(gEfiExitBootServicesEvent);
		gEfiExitBootServicesEvent = NULL;
	}

	// Unhook gRT->SetVariable
	if (mOriginalSetVariable != NULL)
	{
		SetServicePointer(&gRT->Hdr, (VOID**)&gRT->SetVariable, (VOID*)mOriginalSetVariable);
		mOriginalSetVariable = NULL;
	}

	// Unhook gBS->LoadImage
	if (mOriginalLoadImage != NULL)
	{
		SetServicePointer(&gBS->Hdr, (VOID**)&gBS->LoadImage, (VOID*)mOriginalLoadImage);
		mOriginalLoadImage = NULL;
	}

	if (mOriginalStartImage != NULL)
	{
		SetServicePointer(&gBS->Hdr, (VOID**)&gBS->StartImage, (VOID*)mOriginalStartImage);
		mOriginalStartImage = NULL;
	}

	// Unhook gRT->GetVariable
	UninstallSecureBootHook();

	return EFI_SUCCESS;
}

// 
// Main entry point
// 
EFI_STATUS
EFIAPI
EfiMirageInitialize(
	IN EFI_HANDLE ImageHandle,
	IN EFI_SYSTEM_TABLE *SystemTable
	)
{
	ASSERT(ImageHandle == gImageHandle);

	// Check if we're not already loaded.
	EFIMIRAGE_DRIVER_PROTOCOL* EfiMirageDriverProtocol;
	EFI_STATUS Status = gBS->LocateProtocol(&gEfiMirageDriverProtocolGuid,
											NULL,
											(VOID**)&EfiMirageDriverProtocol);
	if (Status != EFI_NOT_FOUND)
	{
		Print(L"An instance of the driver is already loaded.\r\n");
		return EFI_ALREADY_STARTED;
	}

	//
	// Install supported EFI version protocol
	//
	Status = gBS->InstallMultipleProtocolInterfaces(&gImageHandle,
													&gEfiDriverSupportedEfiVersionProtocolGuid,
													&gEfiMirageSupportedEfiVersion,
													NULL);
	if (EFI_ERROR(Status))
	{
		Print(L"Failed to install EFI Driver Supported Version protocol. Error: %llx (%r)\r\n", Status, Status);
		return Status;
	}

	//
	// Query the console input handle for the Simple Text Input Ex protocol
	//
	gBS->HandleProtocol(gST->ConsoleInHandle, &gEfiSimpleTextInputExProtocolGuid, (VOID **)&gTextInputEx);

	//
	// Install EfiMirage driver protocol
	//
	Status = gBS->InstallProtocolInterface(&gImageHandle,
											&gEfiMirageDriverProtocolGuid,
											EFI_NATIVE_INTERFACE,
											&gEfiMirageDriverProtocol);
	if (EFI_ERROR(Status))
		goto Exit;

	//
	// Clear screen and print header
	//
	CONST INT32 OriginalAttribute = SetConsoleTextColour(EFI_GREEN, TRUE);
	Print(L"\r\n\r\n");
	Print(L"%S", EFIMIRAGE_TITLE);
	gST->ConOut->SetAttribute(gST->ConOut, OriginalAttribute);

	EFI_LOADED_IMAGE_PROTOCOL *LocalImageInfo;
	Status = gBS->OpenProtocol(gImageHandle,
								&gEfiLoadedImageProtocolGuid,
								(VOID**)&LocalImageInfo,
								gImageHandle,
								NULL,
								EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(Status))
		goto Exit;

	PrintLoadedImageInfo(LocalImageInfo);

	//
	// Hook gBS->LoadImage
	//
	mOriginalLoadImage = (EFI_IMAGE_LOAD)SetServicePointer(&gBS->Hdr, (VOID**)&gBS->LoadImage, (VOID*)&HookedLoadImage);
	Print(L"Hooked gBS->LoadImage: 0x%p -> 0x%p\r\n", (VOID*)mOriginalLoadImage, (VOID*)&HookedLoadImage);

	mOriginalStartImage = (EFI_IMAGE_START)SetServicePointer(&gBS->Hdr, (VOID**)&gBS->StartImage, (VOID*)&HookedStartImage);

	//
	// Hook gRT->SetVariable
	//
	mOriginalSetVariable = (EFI_SET_VARIABLE)SetServicePointer(&gRT->Hdr, (VOID**)&gRT->SetVariable, (VOID*)&HookedSetVariable);
	Print(L"Hooked gRT->SetVariable: 0x%p -> 0x%p\r\n", (VOID*)mOriginalSetVariable, (VOID*)&HookedSetVariable);

	//
	// Hook gRT->GetVariable for Secure Boot emulation. Installed unconditionally; it is active from
	// the start because the default configuration enables it, and stays a pass-through only while a
	// configuration leaves gDriverConfig.FakeSecureBoot off.
	//
	Status = InstallSecureBootHook();
	if (EFI_ERROR(Status))
		goto Exit;

	// Register notification callback for ExitBootServices()
	Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,
								TPL_NOTIFY,
								ExitBootServicesEvent,
								NULL,
								&gEfiEventExitBootServicesGuid,
								&gEfiExitBootServicesEvent);
	if (EFI_ERROR(Status))
		goto Exit;

	// Register notification callback for SetVirtualAddressMap()
	Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,
								TPL_NOTIFY,
								SetVirtualAddressMapEvent,
								NULL,
								&gEfiEventVirtualAddressChangeGuid,
								&gEfiVirtualNotifyEvent);
	if (EFI_ERROR(Status))
		goto Exit;

	// Initialize the global kernel patch info struct.
	gKernelPatchInfo.Status = EFI_SUCCESS;
	gKernelPatchInfo.BufferSize = 0;
	SetMem64(gKernelPatchInfo.Buffer, sizeof(gKernelPatchInfo.Buffer), 0ULL);
	gKernelPatchInfo.WinloadBuildNumber = 0;
	gKernelPatchInfo.KernelBuildNumber = 0;
	gKernelPatchInfo.KernelBase = NULL;

	// Keep the driver identity visible before the patch-stage output begins.
	RtlSleep(1500);

Exit:
	if (EFI_ERROR(Status))
	{
		Print(L"\r\nEfiMirageDxe initialization failed with status %llx (%r)\r\n", Status, Status);

		// Because we do not use the driver binding protocol, recovering from a failed load is simple.
		// We can just call the unload function, which will only unload that which was actually installed.
		EfiMirageUnload(gImageHandle);
	}
	return Status;
}
