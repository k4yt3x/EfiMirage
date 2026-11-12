/** @file
  Secure Boot emulation self-test.

  Booted as \EFI\BOOT\BOOTX64.EFI by scripts/test/test-secureboot.sh under OVMF with no
  platform key, where the firmware reports SecureBoot=0 and SetupMode=1. Loads the driver
  sitting next to it, configures emulation, and asserts the emulated state and the UEFI
  GetVariable contract for every call shape a caller may use.

  Prints one PASS/FAIL line per assertion and a RESULT line, then exits nonzero if any
  assertion failed.
**/

#include <Uefi.h>

#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EfiGuard.h>
#include <Protocol/LoadedImage.h>

#define DRIVER_PATH L"\\EFI\\BOOT\\EfiGuardDxe.efi"

STATIC UINTN mPassed = 0;
STATIC UINTN mFailed = 0;

STATIC
VOID
Check(
	IN BOOLEAN Condition,
	IN CONST CHAR16* Description
	)
{
	if (Condition)
	{
		++mPassed;
		Print(L"[PASS] %s\r\n", Description);
	}
	else
	{
		++mFailed;
		Print(L"[FAIL] %s\r\n", Description);
	}
}

STATIC
EFI_STATUS
ReadFakeVariable(
	IN CONST CHAR16* Name,
	OUT UINT32* Attributes OPTIONAL,
	IN OUT UINTN* DataSize,
	OUT VOID* Data OPTIONAL
	)
{
	return gRT->GetVariable((CHAR16*)Name, &gEfiGlobalVariableGuid, Attributes, DataSize, Data);
}

//
// Loads and starts the driver from the volume this application was loaded from.
//
STATIC
EFI_STATUS
LoadDriver(
	OUT EFI_HANDLE* DriverHandle
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

	Status = gBS->LoadImage(FALSE, gImageHandle, FilePath, NULL, 0, DriverHandle);
	FreePool(FilePath);
	if (EFI_ERROR(Status))
		return Status;

	return gBS->StartImage(*DriverHandle, NULL, NULL);
}

STATIC
VOID
ConfigureDriver(
	IN EFIGUARD_DRIVER_PROTOCOL* Protocol,
	IN EFIGUARD_DSE_BYPASS_TYPE BypassMethod,
	IN BOOLEAN FakeSecureBoot,
	IN UINT32 Version,
	OUT EFI_STATUS* Status
	)
{
	EFIGUARD_CONFIGURATION_DATA ConfigData = {
		BypassMethod,
		FALSE,
		FakeSecureBoot,
		{ 0, 0 },
		Version
	};
	*Status = Protocol->Configure(&ConfigData);
}

EFI_STATUS
EFIAPI
UefiMain(
	IN EFI_HANDLE ImageHandle,
	IN EFI_SYSTEM_TABLE* SystemTable
	)
{
	Print(L"\r\n=== Secure Boot emulation self-test ===\r\n");

	// The firmware must genuinely be in Secure Boot off / setup mode for this test to mean anything.
	UINT8 Value = 0;
	UINT32 Attributes = 0;
	UINTN Size = sizeof(Value);
	EFI_STATUS Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, &Attributes, &Size, &Value);
	Print(L"SecureBoot baseline: status %llx, value %u, size %u\r\n", (UINT64)Status, Value, Size);
	BOOLEAN RealSecureBootOff = EFI_ERROR(Status) || Value == 0;
	Value = 0xFF;
	Size = sizeof(Value);
	Status = ReadFakeVariable(EFI_SETUP_MODE_NAME, &Attributes, &Size, &Value);
	Print(L"SetupMode baseline: status %llx, value %u, size %u\r\n", (UINT64)Status, Value, Size);
	BOOLEAN RealSetupModeOn = EFI_ERROR(Status) || Value != 0;
	Print(L"Firmware before configuration: SecureBoot off %u, SetupMode on %u\r\n", RealSecureBootOff, RealSetupModeOn);
	Check(RealSecureBootOff && RealSetupModeOn, L"firmware is not already in secure boot mode");

	// Load the driver under test
	EFI_HANDLE DriverHandle = NULL;
	Status = LoadDriver(&DriverHandle);
	Print(L"Driver load status: %llx\r\n", (UINT64)Status);

	// A retried boot starts an already loaded driver; that is not a failure here.
	EFIGUARD_DRIVER_PROTOCOL* Protocol = NULL;
	Status = gBS->LocateProtocol(&gEfiGuardDriverProtocolGuid, NULL, (VOID**)&Protocol);
	Check(!EFI_ERROR(Status) && Protocol != NULL, L"driver loaded and protocol located");
	if (Protocol == NULL)
		goto Done;

	// Emulation must already be on from the default configuration alone
	Value = 0xFF;
	Size = sizeof(Value);
	Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, NULL, &Size, &Value);
	Check(!EFI_ERROR(Status) && Value == 1, L"emulation is on by default, before any configuration");
	Value = 0xFF;
	Size = sizeof(Value);
	Status = ReadFakeVariable(EFI_SETUP_MODE_NAME, NULL, &Size, &Value);
	Check(!EFI_ERROR(Status) && Value == 0, L"SetupMode is faked by default too");

	// A version mismatch must be refused rather than misread
	EFI_STATUS ConfigureStatus;
	ConfigureDriver(Protocol, DSE_DISABLE_NONE, TRUE,
		EFIGUARD_CONFIGURATION_DATA_VERSION + 1, &ConfigureStatus);
	Check(ConfigureStatus == EFI_INCOMPATIBLE_VERSION, L"wrong configuration version is refused");

	// An out-of-range DSE bypass method must be refused
	ConfigureDriver(Protocol, (EFIGUARD_DSE_BYPASS_TYPE)99, TRUE,
		EFIGUARD_CONFIGURATION_DATA_VERSION, &ConfigureStatus);
	Check(ConfigureStatus == EFI_INVALID_PARAMETER, L"out-of-range DSE bypass method is refused");

	// Enable emulation
	ConfigureDriver(Protocol, DSE_DISABLE_NONE, TRUE,
		EFIGUARD_CONFIGURATION_DATA_VERSION, &ConfigureStatus);
	Check(ConfigureStatus == EFI_SUCCESS, L"configuration accepted");

	// SecureBoot now reads as 1 with the attributes from the specification
	Value = 0xFF;
	Attributes = 0;
	Size = sizeof(Value);
	Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, &Attributes, &Size, &Value);
	Check(!EFI_ERROR(Status) && Value == 1 && Size == sizeof(Value), L"SecureBoot reads as enabled");
	Check(Attributes == (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS),
		L"SecureBoot attributes are BS+RT");

	// SetupMode reads as 0 so the state is not self-contradictory
	Value = 0xFF;
	Size = sizeof(Value);
	Status = ReadFakeVariable(EFI_SETUP_MODE_NAME, NULL, &Size, &Value);
	Check(!EFI_ERROR(Status) && Value == 0, L"SetupMode reads as user mode");

	// Size query with a NULL buffer reports the required size
	Size = 0;
	Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, NULL, &Size, NULL);
	Check(Status == EFI_BUFFER_TOO_SMALL && Size == sizeof(UINT8), L"NULL buffer reports size and EFI_BUFFER_TOO_SMALL");

	// A buffer that is too small is rejected the same way
	Value = 0xFF;
	Attributes = 0;
	Size = 0;
	Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, &Attributes, &Size, &Value);
	Check(Status == EFI_BUFFER_TOO_SMALL && Size == sizeof(UINT8) && Value == 0xFF,
		L"undersized buffer is rejected and left untouched");

	// An oversized buffer gets one byte and the real size
	UINT8 BigBuffer[8];
	BigBuffer[0] = 0xFF;
	Size = sizeof(BigBuffer);
	Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, NULL, &Size, BigBuffer);
	Check(!EFI_ERROR(Status) && Size == sizeof(UINT8) && BigBuffer[0] == 1, L"oversized buffer is clamped to one byte");

	// A NULL size pointer is a caller error
	Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, NULL, NULL, &Value);
	Check(Status == EFI_INVALID_PARAMETER, L"NULL size pointer returns EFI_INVALID_PARAMETER");

	// The hook is keyed on the vendor GUID: the same name under another GUID must fall through
	EFI_GUID OtherGuid = { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } };
	Value = 0xFF;
	Size = sizeof(Value);
	Status = gRT->GetVariable((CHAR16*)EFI_SECURE_BOOT_MODE_NAME, &OtherGuid, NULL, &Size, &Value);
	Check(Status == EFI_NOT_FOUND, L"SecureBoot under a foreign vendor GUID falls through to the firmware");

	// Unrelated variables are passed through unchanged
	UINT16 BootCurrent = 0;
	Size = sizeof(BootCurrent);
	Status = gRT->GetVariable(L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &Size, &BootCurrent);
	Check(!EFI_ERROR(Status) && Size == sizeof(BootCurrent), L"unrelated variables pass through unchanged");

	// Turning the feature back off restores the real values
	ConfigureDriver(Protocol, DSE_DISABLE_NONE, FALSE,
		EFIGUARD_CONFIGURATION_DATA_VERSION, &ConfigureStatus);
	Check(ConfigureStatus == EFI_SUCCESS, L"configuration with emulation off accepted");
	Value = 0xFF;
	Size = sizeof(Value);
	Status = ReadFakeVariable(EFI_SECURE_BOOT_MODE_NAME, NULL, &Size, &Value);
	Check(EFI_ERROR(Status) || Value == 0, L"SecureBoot no longer reads as enabled once turned off");

Done:
	Print(L"RESULT: %u passed, %u failed\r\n", mPassed, mFailed);
	gBS->Exit(ImageHandle, mFailed == 0 ? EFI_SUCCESS : EFI_ABORTED, 0, NULL);

	return EFI_SUCCESS;
}
