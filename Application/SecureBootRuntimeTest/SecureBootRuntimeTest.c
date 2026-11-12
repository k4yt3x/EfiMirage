/** @file
  Secure Boot emulation runtime test.

  Booted by scripts/test/test-secureboot.sh in place of SecureBootTest when the harness is
  asked for the runtime variant. Uses an identity virtual mapping so it does not need a
  real loader, but exercises the parts that only break after ExitBootServices(): pointer
  conversion in the SetVirtualAddressMap notification and GetVariable calls into the
  converted firmware pointer.

  Boot services are gone from the midpoint on, so this test reports through ConOut and
  reboots the machine when it is done.
**/

#include <Uefi.h>

#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Library/DevicePathLib.h>
#include <Library/CpuLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EfiGuard.h>
#include <Protocol/LoadedImage.h>

#define DRIVER_PATH L"\\EFI\\BOOT\\EfiGuardDxe.efi"

STATIC UINTN mPassed = 0;
STATIC UINTN mFailed = 0;
STATIC UINTN mTableBefore = 0;
STATIC UINTN mGetVariableBefore = 0;

//
// ConOut does not survive ExitBootServices() reliably (its device stack is boot services
// memory), so everything is reported straight to COM1 instead.
//
#define COM1 0x3F8

STATIC
VOID
SerialWrite(
	IN CONST CHAR8* Text
	)
{
	for (; *Text != '\0'; ++Text)
	{
		while ((IoRead8(COM1 + 5) & 0x20) == 0)
			CpuPause();
		IoWrite8(COM1, (UINT8)*Text);
	}
}

STATIC
VOID
SerialWriteNumber(
	IN UINTN Value
	)
{
	CHAR8 Digits[21];
	UINTN Index = 0;
	if (Value == 0)
	{
		SerialWrite("0");
		return;
	}
	while (Value != 0)
	{
		Digits[Index++] = (CHAR8)('0' + Value % 10);
		Value /= 10;
	}
	while (Index > 0)
	{
		CHAR8 Character = Digits[--Index];
		while ((IoRead8(COM1 + 5) & 0x20) == 0)
			CpuPause();
		IoWrite8(COM1, (UINT8)Character);
	}
}

STATIC
VOID
SerialWriteHex(
	IN UINTN Value
	)
{
	CHAR8 Digits[17];
	for (UINTN i = 0; i < 16; ++i)
	{
		Digits[i] = "0123456789ABCDEF"[(Value >> ((15 - i) * 4)) & 0xF];
	}
	Digits[16] = '\0';
	SerialWrite("0x");
	SerialWrite(Digits);
}

STATIC
VOID
Check(
	IN BOOLEAN Condition,
	IN CONST CHAR16* Description
	)
{
	// Descriptions are ASCII so they can go out over the serial line unchanged
	SerialWrite(Condition ? "[PASS] " : "[FAIL] ");
	for (; *Description != '\0'; ++Description)
	{
		CHAR8 Character = (CHAR8)*Description;
		while ((IoRead8(COM1 + 5) & 0x20) == 0)
			CpuPause();
		IoWrite8(COM1, (UINT8)Character);
	}
	SerialWrite("\r\n");

	if (Condition)
		++mPassed;
	else
		++mFailed;
}

STATIC
EFI_STATUS
LoadDriver(
	VOID
	)
{
	EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
	EFI_STATUS Status = gBS->OpenProtocol(gImageHandle,
										&gEfiLoadedImageProtocolGuid,
										(VOID**)&LoadedImage,
										gImageHandle,
										NULL,
										EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(Status))
		return Status;

	EFI_DEVICE_PATH_PROTOCOL* FilePath = FileDevicePath(LoadedImage->DeviceHandle, DRIVER_PATH);
	if (FilePath == NULL)
		return EFI_OUT_OF_RESOURCES;

	EFI_HANDLE DriverHandle = NULL;
	Status = gBS->LoadImage(FALSE, gImageHandle, FilePath, NULL, 0, &DriverHandle);
	FreePool(FilePath);
	if (EFI_ERROR(Status))
		return Status;

	return gBS->StartImage(DriverHandle, NULL, NULL);
}

//
// Leaves boot services and installs an identity virtual mapping, like a boot loader would.
//
STATIC
EFI_STATUS
EnterVirtualMode(
	VOID
	)
{
	UINTN MemoryMapSize = 0;
	UINTN MapKey = 0;
	UINTN DescriptorSize = 0;
	UINT32 DescriptorVersion = 0;

	EFI_STATUS Status = gBS->GetMemoryMap(&MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
	if (Status != EFI_BUFFER_TOO_SMALL)
		return Status;

	// Leave room for the entries ExitBootServices() itself may add
	MemoryMapSize += 4 * DescriptorSize;
	EFI_MEMORY_DESCRIPTOR* MemoryMap = AllocatePool(MemoryMapSize);
	if (MemoryMap == NULL)
		return EFI_OUT_OF_RESOURCES;

	Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
	if (EFI_ERROR(Status))
		return Status;

	Status = gBS->ExitBootServices(gImageHandle, MapKey);
	if (EFI_ERROR(Status))
		return Status;

	// Identity map: keep every host address where the firmware already has it
	UINTN EntryCount = MemoryMapSize / DescriptorSize;
	for (UINTN i = 0; i < EntryCount; ++i)
	{
		EFI_MEMORY_DESCRIPTOR* Entry = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + i * DescriptorSize);
		Entry->VirtualStart = Entry->PhysicalStart;
	}

	return gRT->SetVirtualAddressMap(MemoryMapSize, DescriptorSize, DescriptorVersion, MemoryMap);
}

EFI_STATUS
EFIAPI
UefiMain(
	IN EFI_HANDLE ImageHandle,
	IN EFI_SYSTEM_TABLE* SystemTable
	)
{
	SerialWrite("\r\n=== Secure Boot emulation runtime test ===\r\n");

	EFI_STATUS Status = LoadDriver();
	EFIGUARD_DRIVER_PROTOCOL* Protocol = NULL;
	if (!EFI_ERROR(Status))
		Status = gBS->LocateProtocol(&gEfiGuardDriverProtocolGuid, NULL, (VOID**)&Protocol);
	SerialWrite("driver ready\r\n");
	if (Protocol == NULL)
	{
		SerialWrite("RESULT: ");
		SerialWriteNumber(mPassed);
		SerialWrite(" passed, ");
		SerialWriteNumber(++mFailed);
		SerialWrite(" failed\r\n");
		gBS->Exit(ImageHandle, EFI_ABORTED, 0, NULL);
	}

	// The default is on, so it must already be emulated before Configure() runs
	UINT8 DefaultValue = 0xFF;
	UINTN DefaultSize = sizeof(DefaultValue);
	Status = gRT->GetVariable((CHAR16*)EFI_SECURE_BOOT_MODE_NAME, &gEfiGlobalVariableGuid, NULL, &DefaultSize, &DefaultValue);
	Check(!EFI_ERROR(Status) && DefaultValue == 1, L"emulation is on by default, before any configuration");

	EFIGUARD_CONFIGURATION_DATA ConfigData = {
		DSE_DISABLE_NONE,
		FALSE,
		TRUE,
		{ 0, 0 },
		EFIGUARD_CONFIGURATION_DATA_VERSION
	};
	Status = Protocol->Configure(&ConfigData);
	Check(!EFI_ERROR(Status), L"emulation configured before leaving boot services");

	mTableBefore = (UINTN)gRT;
	mGetVariableBefore = (UINTN)gRT->GetVariable;
	Status = EnterVirtualMode();
	Check(!EFI_ERROR(Status), L"ExitBootServices and SetVirtualAddressMap succeeded");
	if (EFI_ERROR(Status))
	{
		SerialWrite("RESULT: ");
		SerialWriteNumber(mPassed);
		SerialWrite(" passed, ");
		SerialWriteNumber(++mFailed);
		SerialWrite(" failed\r\n");
		gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
	}

	// DxeCore installs a new runtime services table on SetVirtualAddressMap; pick it up or
	// every call below would go to the stale physical table and bypass the hook.
	gRT = gST->RuntimeServices;

	SerialWrite("table before EBS was 0x");
	SerialWriteHex(mTableBefore);
	SerialWrite(", GetVariable was 0x");
	SerialWriteHex(mGetVariableBefore);
	SerialWrite("\r\n");
	SerialWrite("table now 0x");
	SerialWriteHex((UINTN)gRT);
	SerialWrite(", GetVariable now 0x");
	SerialWriteHex((UINTN)gRT->GetVariable);
	SerialWrite("\r\n");

	// Everything below runs at runtime: the original GetVariable pointer has been converted
	// and the firmware image must still be reachable.
	UINT8 Value = 0xFF;
	UINT32 Attributes = 0;
	UINTN Size = sizeof(Value);
	Status = gRT->GetVariable((CHAR16*)EFI_SECURE_BOOT_MODE_NAME, &gEfiGlobalVariableGuid, &Attributes, &Size, &Value);
	Check(!EFI_ERROR(Status) && Value == 1, L"SecureBoot still reads as enabled at runtime");
	Check(Attributes == (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS),
		L"runtime attributes are still BS+RT");

	Value = 0xFF;
	Size = sizeof(Value);
	Status = gRT->GetVariable((CHAR16*)EFI_SETUP_MODE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, &Value);
	Check(!EFI_ERROR(Status) && Value == 0, L"SetupMode still reads as user mode at runtime");

	// The pass-through path must survive conversion as well
	Size = 0;
	Status = gRT->GetVariable(L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &Size, NULL);
	Check(Status == EFI_BUFFER_TOO_SMALL && Size == sizeof(UINT16), L"firmware variables still pass through at runtime");

	SerialWrite("RESULT: ");
	SerialWriteNumber(mPassed);
	SerialWrite(" passed, ");
	SerialWriteNumber(mFailed);
	SerialWrite(" failed\r\n");
	gRT->ResetSystem(mFailed == 0 ? EfiResetShutdown : EfiResetCold, EFI_SUCCESS, 0, NULL);

	return EFI_SUCCESS;
}
