/** @file
  Secure Boot emulation runtime test.

  Booted by scripts/test/test-secureboot.sh in place of SecureBootTest when the harness is
  asked for the runtime variant. Uses an identity virtual mapping so it does not need a
  real loader, but exercises the parts that only break after ExitBootServices(): pointer
  conversion in the SetVirtualAddressMap notification and GetVariable calls into the
  converted firmware pointer.

  Boot services are gone from the midpoint on and ConOut does not survive that, so the test
  reports straight to COM1 and shuts the machine down when it is done.
**/

#include <Uefi.h>

#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
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
// Runtime regions are mapped at a fixed non-zero offset by duplicating the low PML4 entry into
// a higher-half slot. With an identity mapping ConvertPointer() is a no-op and a forgotten
// conversion in the driver would go unnoticed; with this offset a stale physical pointer to
// runtime code stops working once the identity alias for that code is removed below.
//
#define VIRTUAL_OFFSET		(0xFFFF800000000000ULL)
#define VIRTUAL_PML4_INDEX	256
#define PAGE_SIZE_4K		0x1000ULL
#define PAGE_SIZE_2M		0x200000ULL
#define CR0_WP				0x00010000ULL

STATIC UINT8* mPagePool = NULL;
STATIC UINTN mPagePoolUsed = 0;
STATIC UINTN mPagePoolPages = 0;

STATIC
VOID*
EFIAPI
PoolPage(
	VOID
	)
{
	if (mPagePoolUsed >= mPagePoolPages)
		return NULL;
	VOID* Page = mPagePool + mPagePoolUsed * PAGE_SIZE_4K;
	++mPagePoolUsed;
	SetMem(Page, PAGE_SIZE_4K, 0);
	return Page;
}

//
// The firmware maps its page tables read-only, so writes to them need CR0.WP cleared.
//
STATIC
UINTN
EFIAPI
BeginPageTableWrite(
	VOID
	)
{
	CONST UINTN Cr0 = AsmReadCr0();
	if ((Cr0 & CR0_WP) != 0)
		AsmWriteCr0(Cr0 & ~CR0_WP);
	return Cr0;
}

STATIC
VOID
EFIAPI
EndPageTableWrite(
	IN UINTN Cr0
	)
{
	AsmWriteCr0(Cr0);
}

STATIC
BOOLEAN
EFIAPI
IsPresent(
	IN UINT64 Entry
	)
{
	return (Entry & 1) != 0;
}

//
// Removes the low (identity) mapping for [PhysicalStart, PhysicalStart + Size), splitting 2 MiB
// pages into 4 KiB pages as needed out of a pool allocated before ExitBootServices().
//
STATIC
BOOLEAN
EFIAPI
UnmapIdentityRange(
	IN UINT64 PhysicalStart,
	IN UINT64 Size
	)
{
	UINT64* Pml4 = (UINT64*)(UINTN)(AsmReadCr3() & ~0xFFFULL);
	if (!IsPresent(Pml4[0]))
		return FALSE;

	UINT64* Pdpt = (UINT64*)(UINTN)(Pml4[0] & ~0xFFFULL);
	UINT64 End = PhysicalStart + Size;
	CONST UINTN SavedCr0 = BeginPageTableWrite();
	for (UINT64 Address = PhysicalStart & ~(PAGE_SIZE_4K - 1); Address < End; Address += PAGE_SIZE_4K)
	{
		UINT64* PdptEntry = &Pdpt[(Address >> 30) & 0x1FF];
		if (!IsPresent(*PdptEntry) || (*PdptEntry & (1ULL << 7)) != 0)
			continue;

		UINT64* Pd = (UINT64*)(UINTN)(*PdptEntry & ~0xFFFULL);
		UINT64* PdEntry = &Pd[(Address >> 21) & 0x1FF];
		if (!IsPresent(*PdEntry))
			continue;

		if ((*PdEntry & (1ULL << 7)) != 0)
		{
			UINT64* PageTable = PoolPage();
			if (PageTable == NULL)
				return FALSE;
			CONST UINT64 Base = *PdEntry & ~(PAGE_SIZE_2M - 1);
			CONST UINT64 Flags = *PdEntry & 0xFFF;
			for (UINTN i = 0; i < 512; ++i)
				PageTable[i] = (Base + i * PAGE_SIZE_4K) | Flags;
			*PdEntry = (UINT64)(UINTN)PageTable | 0x3;
		}

		UINT64* PageTable = (UINT64*)(UINTN)(*PdEntry & ~0xFFFULL);
		PageTable[(Address >> 12) & 0x1FF] = 0;
	}

	EndPageTableWrite(SavedCr0);
	return TRUE;
}

//
// Builds an independent page table tree under PML4[VIRTUAL_PML4_INDEX] that maps the runtime
// ranges at PhysicalStart + VIRTUAL_OFFSET. Only those ranges are mapped; anything else under the
// high half stays not present.
//
STATIC
BOOLEAN
EFIAPI
BuildHighHalfMapping(
	IN EFI_MEMORY_DESCRIPTOR* MemoryMap,
	IN UINTN EntryCount,
	IN UINTN DescriptorSize
	)
{
	UINT64* Pml4 = (UINT64*)(UINTN)(AsmReadCr3() & ~0xFFFULL);
	CONST UINTN SavedCr0 = BeginPageTableWrite();

	UINT64* Pdpt = PoolPage();
	if (Pdpt == NULL)
	{
		EndPageTableWrite(SavedCr0);
		return FALSE;
	}
	Pml4[VIRTUAL_PML4_INDEX] = (UINT64)(UINTN)Pdpt | 0x3;

	BOOLEAN Result = TRUE;
	for (UINTN i = 0; i < EntryCount && Result; ++i)
	{
		EFI_MEMORY_DESCRIPTOR* Entry = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + i * DescriptorSize);
		if (Entry->Type != EfiRuntimeServicesCode && Entry->Type != EfiRuntimeServicesData)
			continue;

		UINT64 End = Entry->PhysicalStart + Entry->NumberOfPages * PAGE_SIZE_4K;
		for (UINT64 Address = Entry->PhysicalStart & ~(PAGE_SIZE_4K - 1); Address < End; Address += PAGE_SIZE_4K)
		{
			CONST UINT64 Virtual = Address + VIRTUAL_OFFSET;
			CONST UINTN PdptIndex = (UINTN)((Virtual >> 30) & 0x1FF);
			CONST UINTN PdIndex = (UINTN)((Virtual >> 21) & 0x1FF);
			CONST UINTN PtIndex = (UINTN)((Virtual >> 12) & 0x1FF);

			if (!IsPresent(Pdpt[PdptIndex]))
			{
				UINT64* NewPd = PoolPage();
				if (NewPd == NULL)
				{
					Result = FALSE;
					break;
				}
				Pdpt[PdptIndex] = (UINT64)(UINTN)NewPd | 0x3;
			}
			UINT64* Pd = (UINT64*)(UINTN)(Pdpt[PdptIndex] & ~0xFFFULL);

			if (!IsPresent(Pd[PdIndex]))
			{
				UINT64* NewPt = PoolPage();
				if (NewPt == NULL)
				{
					Result = FALSE;
					break;
				}
				Pd[PdIndex] = (UINT64)(UINTN)NewPt | 0x3;
			}
			UINT64* Pt = (UINT64*)(UINTN)(Pd[PdIndex] & ~0xFFFULL);

			Pt[PtIndex] = Address | 0x3;
		}
	}

	EndPageTableWrite(SavedCr0);
	AsmWriteCr3(AsmReadCr3());
	return Result;
}

//
// Leaves boot services and switches to a virtual map where runtime regions live at VIRTUAL_OFFSET.
//
STATIC
EFI_STATUS
EnterVirtualMode(
	OUT EFI_MEMORY_DESCRIPTOR** VirtualMap,
	OUT UINTN* VirtualMapSize,
	OUT UINTN* VirtualMapDescriptorSize
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
	MemoryMapSize += 8 * DescriptorSize;
	EFI_MEMORY_DESCRIPTOR* MemoryMap = AllocatePool(MemoryMapSize);
	if (MemoryMap == NULL)
		return EFI_OUT_OF_RESOURCES;

	// Page tables for splitting 2 MiB pages must come from boot services
	mPagePoolPages = 32;
	mPagePool = AllocatePages(mPagePoolPages);
	if (mPagePool == NULL)
		return EFI_OUT_OF_RESOURCES;

	// Five level paging would need a different slot; skip the aliasing rather than guess
	if ((AsmReadCr4() & (1ULL << 12)) != 0)
		return EFI_UNSUPPORTED;

	UINT64* Pml4 = (UINT64*)(UINTN)(AsmReadCr3() & ~0xFFFULL);
	if (IsPresent(Pml4[VIRTUAL_PML4_INDEX]))
		return EFI_ALREADY_STARTED;

	Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
	if (EFI_ERROR(Status))
		return Status;

	Status = gBS->ExitBootServices(gImageHandle, MapKey);
	if (EFI_ERROR(Status))
		return Status;

	// Runtime code and data move to the alias; everything else, including this application,
	// stays where it is.
	UINTN EntryCount = MemoryMapSize / DescriptorSize;
	for (UINTN i = 0; i < EntryCount; ++i)
	{
		EFI_MEMORY_DESCRIPTOR* Entry = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + i * DescriptorSize);
		Entry->VirtualStart = (Entry->Type == EfiRuntimeServicesCode || Entry->Type == EfiRuntimeServicesData)
			? Entry->PhysicalStart + VIRTUAL_OFFSET
			: Entry->PhysicalStart;
	}

	// Give the high half its own page table tree before anything is unmapped from the low one,
	// otherwise clearing an identity entry would clear the aliased entry with it.
	if (!BuildHighHalfMapping(MemoryMap, EntryCount, DescriptorSize))
		return EFI_OUT_OF_RESOURCES;

	Status = gRT->SetVirtualAddressMap(MemoryMapSize, DescriptorSize, DescriptorVersion, MemoryMap);
	if (EFI_ERROR(Status))
		return Status;

	// Drop the identity alias for runtime code so a pointer that was never converted faults
	// instead of silently still working.
	for (UINTN i = 0; i < EntryCount; ++i)
	{
		EFI_MEMORY_DESCRIPTOR* Entry = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + i * DescriptorSize);
		if (Entry->Type == EfiRuntimeServicesCode)
			UnmapIdentityRange(Entry->PhysicalStart, Entry->NumberOfPages * PAGE_SIZE_4K);
	}
	AsmWriteCr3(AsmReadCr3());

	*VirtualMap = MemoryMap;
	*VirtualMapSize = MemoryMapSize;
	*VirtualMapDescriptorSize = DescriptorSize;
	return EFI_SUCCESS;
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
	Check(Protocol != NULL, L"driver loaded and protocol located");
	if (Protocol == NULL)
	{
		SerialWrite("RESULT: ");
		SerialWriteNumber(mPassed);
		SerialWrite(" passed, ");
		SerialWriteNumber(mFailed);
		SerialWrite(" failed\r\n");
		gBS->Exit(ImageHandle, EFI_ABORTED, 0, NULL);
	}

	// The default is on, so it must already be emulated before Configure() runs
	UINT8 DefaultValue = 0xFF;
	UINTN DefaultSize = sizeof(DefaultValue);
	Status = gRT->GetVariable((CHAR16*)EFI_SECURE_BOOT_MODE_NAME, &gEfiGlobalVariableGuid, NULL, &DefaultSize, &DefaultValue);
	Check(!EFI_ERROR(Status) && DefaultValue == 1, L"emulation is on by default, before any configuration");

	EFIGUARD_CONFIGURATION_DATA ConfigData = {
		EFIGUARD_CONFIGURATION_DATA_VERSION,
		DSE_DISABLE_NONE,
		FALSE,
		TRUE,
		{ 0, 0 }
	};
	Status = Protocol->Configure(&ConfigData);
	Check(!EFI_ERROR(Status), L"emulation configured before leaving boot services");

	mTableBefore = (UINTN)gRT;
	mGetVariableBefore = (UINTN)gRT->GetVariable;
	EFI_MEMORY_DESCRIPTOR* VirtualMap = NULL;
	UINTN VirtualMapSize = 0;
	UINTN VirtualMapDescriptorSize = 0;
	Status = EnterVirtualMode(&VirtualMap, &VirtualMapSize, &VirtualMapDescriptorSize);
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
