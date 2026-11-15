/** @file
  EFI Secure Boot state emulation.

  Windows and the software running on it do not talk to the firmware's Secure Boot
  state directly; they read the SecureBoot and SetupMode EFI variables. This module
  hooks gRT->GetVariable and reports "Secure Boot enabled, user mode" for those
  variables so that everything above the firmware sees a secure boot chain even
  where the firmware has none.

  The hook is installed at driver load and stays installed until the driver is
  unloaded or the image is released at ExitBootServices(). It is transparent when
  gDriverConfig.FakeSecureBoot is FALSE.

  Because winload.efi reads SecureBoot to pick its image validation path before
  ExitBootServices() is called, the hook has to be live during boot services as
  well as at runtime; the flag is therefore consulted per call rather than the hook
  only being installed on demand.
**/

#include "EfiMirageDxe.h"

#include <Guid/ImageAuthentication.h>
#include <Library/BaseMemoryLib.h>

//
// Attributes of every variable this module answers. Real firmware (checked on OVMF) reports
// the secure boot state variables as boot service + runtime access only, without the
// non-volatile bit the specification's global variable table lists, so match that.
//
#define EFIMIRAGE_SECURE_BOOT_VARIABLE_ATTRIBUTES \
	(EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

//
// Original gRT->GetVariable pointer. Only valid while the hook is installed.
//
STATIC EFI_GET_VARIABLE mOriginalGetVariable = NULL;

//
// Whether the hook is in the table. Tracked separately from mOriginalGetVariable because the
// hook is already installed by the time that pointer is known to be usable.
//
STATIC BOOLEAN mHookInstalled = FALSE;

//
// A variable this module answers itself instead of forwarding to the firmware.
//
typedef struct _FAKE_VARIABLE
{
	CONST CHAR16* Name;
	UINT8 Value;		// BOOLEAN-ish: 1 for SecureBoot, 0 for SetupMode
} FAKE_VARIABLE;

//
// Everything Secure Boot related that Windows or a diagnostic tool may consult under the global
// variable GUID. SecureBootEnable is deliberately not here; see the note below the table.
//
STATIC CONST FAKE_VARIABLE mFakeVariables[] =
{
	{ EFI_SECURE_BOOT_MODE_NAME, SECURE_BOOT_MODE_ENABLE },
	{ EFI_SETUP_MODE_NAME,		 USER_MODE }
};

//
// SecureBootEnable is not one of the global variables: firmware that still has it keeps it under
// its own vendor GUID (SecurityPkg's gEfiSecureBootEnableDisableGuid), and Windows does not read it.
// It is therefore deliberately not answered here rather than invented under the wrong GUID, which
// would make GetVariable and GetNextVariableName disagree.
//

//
// Returns the value to report for VariableName, or NULL if this is not a variable we answer.
//
STATIC
CONST FAKE_VARIABLE*
EFIAPI
LookupFakeVariable(
	IN CONST CHAR16* VariableName
	)
{
	if (VariableName == NULL || VariableName[0] == CHAR_NULL)
		return NULL;

	for (UINTN i = 0; i < ARRAY_SIZE(mFakeVariables); ++i)
	{
		CONST UINTN Length = StrLen(mFakeVariables[i].Name);
		if (StrnCmp(VariableName, mFakeVariables[i].Name, Length + 1) == 0)
			return &mFakeVariables[i];
	}

	return NULL;
}

//
// Same contract as EDK2's VariableServiceGetVariable: a buffer that is large enough but has no
// pointer behind it is a caller error and leaves *DataSize and *Attributes alone, a buffer that is
// too small reports the required size, and only a successful read copies the value.
//
STATIC
EFI_STATUS
EFIAPI
ReadFakeVariable(
	IN CONST FAKE_VARIABLE* FakeVariable,
	OUT UINT32* Attributes OPTIONAL,
	IN OUT UINTN* DataSize,
	OUT VOID* Data OPTIONAL
	)
{
	CONST UINTN RequiredSize = sizeof(FakeVariable->Value);
	EFI_STATUS Status;

	if (DataSize == NULL)
		return EFI_INVALID_PARAMETER;

	CONST UINTN BufferSize = *DataSize;
	if (BufferSize >= RequiredSize)
	{
		if (Data == NULL)
			return EFI_INVALID_PARAMETER;

		*(UINT8*)Data = FakeVariable->Value;
		*DataSize = RequiredSize;
		Status = EFI_SUCCESS;
	}
	else
	{
		*DataSize = RequiredSize;
		Status = EFI_BUFFER_TOO_SMALL;
	}

	if (Attributes != NULL)
		*Attributes = EFIMIRAGE_SECURE_BOOT_VARIABLE_ATTRIBUTES;

	return Status;
}

//
// Runtime Services GetVariable hook
//
EFI_STATUS
EFIAPI
HookedGetVariable(
	IN CHAR16* VariableName,
	IN EFI_GUID* VendorGuid,
	OUT UINT32* Attributes OPTIONAL,
	IN OUT UINTN* DataSize,
	OUT VOID* Data OPTIONAL
	)
{
	ASSERT(mOriginalGetVariable != NULL);

	// Only answer while the feature is on and the firmware variable would be readable by this caller.
	// SecureBoot and SetupMode are always present on UEFI 2.3.1+ and are marked boot service + runtime,
	// so there is no case where faking them at runtime but not at boot (or vice versa) would be correct.
	if (gDriverConfig.FakeSecureBoot &&
		VariableName != NULL && VariableName[0] != CHAR_NULL &&
		VendorGuid != NULL && DataSize != NULL &&
		CompareGuid(VendorGuid, &gEfiGlobalVariableGuid))
	{
		CONST FAKE_VARIABLE* FakeVariable = LookupFakeVariable(VariableName);
		if (FakeVariable != NULL)
			return ReadFakeVariable(FakeVariable, Attributes, DataSize, Data);
	}

	return mOriginalGetVariable(VariableName, VendorGuid, Attributes, DataSize, Data);
}

//
// Installs the GetVariable hook. Safe to call when it is already installed.
//
EFI_STATUS
EFIAPI
InstallSecureBootHook(
	VOID
	)
{
	if (mHookInstalled)
		return EFI_SUCCESS;

	ASSERT(gBS != NULL);

	mOriginalGetVariable = (EFI_GET_VARIABLE)SetServicePointer(&gRT->Hdr,
																(VOID**)&gRT->GetVariable,
																(VOID*)&HookedGetVariable);
	mHookInstalled = TRUE;

	if (mOriginalGetVariable == NULL)
		return EFI_NOT_FOUND;

	Print(L"Hooked gRT->GetVariable: 0x%p -> 0x%p\r\n", (VOID*)mOriginalGetVariable, (VOID*)&HookedGetVariable);
	return EFI_SUCCESS;
}

//
// Restores the original GetVariable pointer. Used both on unload and, when the image is about to become
// unreachable, at ExitBootServices().
//
VOID
EFIAPI
UninstallSecureBootHook(
	VOID
	)
{
	if (!mHookInstalled)
		return;

	// gBS is NULL after ExitBootServices(); the table header checksum cannot be recomputed then, which is
	// harmless because nothing reads the runtime table CRC after boot services are gone.
	if (gBS != NULL)
		SetServicePointer(&gRT->Hdr, (VOID**)&gRT->GetVariable, (VOID*)mOriginalGetVariable);
	else
		gRT->GetVariable = mOriginalGetVariable;

	mHookInstalled = FALSE;
	mOriginalGetVariable = NULL;
}

//
// Converts the saved original GetVariable pointer to a virtual address. Must be called from the
// SetVirtualAddressMap() notification while the hook is still installed.
//
VOID
EFIAPI
SecureBootConvertRuntimePointers(
	VOID
	)
{
	if (!mHookInstalled || mOriginalGetVariable == NULL)
		return;

	EFI_STATUS Status = gRT->ConvertPointer(0, (VOID**)&mOriginalGetVariable);
	ASSERT_EFI_ERROR(Status);
}
