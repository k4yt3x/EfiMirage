#include "EfiDSEFix.h"
#include "EfiCompat.h"
#include <ntstatus.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/EfiMirage.h>

// Defined in EfiDSEFix.cpp; EFI_GUID is GUID here, so the C++ type matches.
extern EFI_GUID gEfiMirageStateVariableGuid;
extern EFI_GUID gMicrosoftVendorGuid;

#ifndef KEY_QUERY_VALUE
#define KEY_QUERY_VALUE 0x0001
#endif

extern EFI_GUID gEfiGlobalVariableGuid;

static constexpr PCWCHAR CodeIntegrityOptionNames[] =
{
	L"CODEINTEGRITY_OPTION_ENABLED",
	L"CODEINTEGRITY_OPTION_TESTSIGN",
	L"CODEINTEGRITY_OPTION_UMCI_ENABLED",
	L"CODEINTEGRITY_OPTION_UMCI_AUDITMODE_ENABLED",
	L"CODEINTEGRITY_OPTION_UMCI_EXCLUSIONPATHS_ENABLED",
	L"CODEINTEGRITY_OPTION_TEST_BUILD",
	L"CODEINTEGRITY_OPTION_PREPRODUCTION_BUILD",
	L"CODEINTEGRITY_OPTION_DEBUGMODE_ENABLED",
	L"CODEINTEGRITY_OPTION_FLIGHT_BUILD",
	L"CODEINTEGRITY_OPTION_FLIGHTING_ENABLED",
	L"CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED",
	L"CODEINTEGRITY_OPTION_HVCI_KMCI_AUDITMODE_ENABLED",
	L"CODEINTEGRITY_OPTION_HVCI_KMCI_STRICTMODE_ENABLED",
	L"CODEINTEGRITY_OPTION_HVCI_IUM_ENABLED",
	L"CODEINTEGRITY_OPTION_WHQL_ENFORCEMENT_ENABLED",
	L"CODEINTEGRITY_OPTION_WHQL_AUDITMODE_ENABLED"
};

static
VOID
PrintCodeIntegrityOptions(
	_In_ ULONG CodeIntegrityOptions
	)
{
	for (ULONG i = 0; i < ARRAYSIZE(CodeIntegrityOptionNames); ++i)
	{
		const ULONG Value = 1UL << i;
		if ((CodeIntegrityOptions & Value) != 0)
		{
			Printf(L"\t   0x%04lX: %ls\n", Value, CodeIntegrityOptionNames[i]);
		}
	}
}

NTSTATUS
DumpSystemInformation(
	)
{
	SYSTEM_BOOT_ENVIRONMENT_INFORMATION BootInfo = {};
	NTSTATUS Status = NtQuerySystemInformation(SystemBootEnvironmentInformation,
												&BootInfo,
												sizeof(BootInfo),
												nullptr);
	if (!NT_SUCCESS(Status))
		Printf(L"SystemBootEnvironmentInformation: error %08lX\n\n", Status);
	else
	{
		Printf(L"SystemBootEnvironmentInformation:\n\t- BootIdentifier: ");
		PrintGuid(BootInfo.BootIdentifier);
		Printf(L"\n\t- FirmwareType: %s\n\t- BootFlags: 0x%llX\n\n",
			(BootInfo.FirmwareType == FirmwareTypeUefi ? L"UEFI" : L"BIOS"), BootInfo.BootFlags);
	}

	ULONG Size = 0;
	Status = NtQuerySystemInformation(SystemModuleInformation,
										nullptr,
										0,
										&Size);
	if (Status != STATUS_INFO_LENGTH_MISMATCH)
		Printf(L"SystemModuleInformation: %08lX\n\n", Status);
	else
	{
		const PRTL_PROCESS_MODULES ModuleInfo = static_cast<PRTL_PROCESS_MODULES>(
			RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, 2 * static_cast<SIZE_T>(Size)));
		Status = NtQuerySystemInformation(SystemModuleInformation,
										ModuleInfo,
										2 * Size,
										nullptr);
		if (!NT_SUCCESS(Status))
			Printf(L"SystemModuleInformation: %08lX\n\n", Status);
		else
		{
			const PRTL_PROCESS_MODULE_INFORMATION Ntoskrnl = &ModuleInfo->Modules[0];
			Printf(L"SystemModuleInformation:\n\t- Kernel: %S (%S)\n\n",
				reinterpret_cast<PCCH>(Ntoskrnl->FullPathName + Ntoskrnl->OffsetToFileName),
				reinterpret_cast<PCCH>(Ntoskrnl->FullPathName));
		}
		RtlFreeHeap(RtlProcessHeap(), 0, ModuleInfo);
	}

	SYSTEM_CODEINTEGRITY_INFORMATION CodeIntegrityInfo = { sizeof(SYSTEM_CODEINTEGRITY_INFORMATION) };
	Status = NtQuerySystemInformation(SystemCodeIntegrityInformation,
										&CodeIntegrityInfo,
										sizeof(CodeIntegrityInfo),
										nullptr);
	if (!NT_SUCCESS(Status))
		Printf(L"SystemCodeIntegrityInformation: error %08lX\n\n", Status);
	else
	{
		Printf(L"SystemCodeIntegrityInformation:\n\t- IntegrityOptions: 0x%04lX\n",
			CodeIntegrityInfo.CodeIntegrityOptions);
		PrintCodeIntegrityOptions(CodeIntegrityInfo.CodeIntegrityOptions);
	}

	SYSTEM_KERNEL_DEBUGGER_INFORMATION KernelDebuggerInfo = { 0 };
	Status = NtQuerySystemInformation(SystemKernelDebuggerInformation,
										&KernelDebuggerInfo,
										sizeof(KernelDebuggerInfo),
										nullptr);
	if (!NT_SUCCESS(Status))
		Printf(L"\nSystemKernelDebuggerInformation: error %08lX\n\n", Status);
	else
		Printf(L"\nSystemKernelDebuggerInformation:\n\t- KernelDebuggerEnabled: %hhu\n\t- KernelDebuggerNotPresent: %hhu\n\n",
			KernelDebuggerInfo.KernelDebuggerEnabled, KernelDebuggerInfo.KernelDebuggerNotPresent);

	if ((RtlNtMajorVersion() >= 6 && RtlNtMinorVersion() >= 3) || RtlNtMajorVersion() > 6)
	{
		SYSTEM_KERNEL_DEBUGGER_INFORMATION_EX KernelDebuggerInfoEx = { 0 };
		Status = NtQuerySystemInformation(SystemKernelDebuggerInformationEx,
											&KernelDebuggerInfoEx,
											sizeof(KernelDebuggerInfoEx),
											nullptr);
		if (!NT_SUCCESS(Status))
			Printf(L"SystemKernelDebuggerInformationEx: error %08lX\n\n", Status);
		else
			Printf(L"SystemKernelDebuggerInformationEx:\n\t- DebuggerAllowed: %hhu\n\t- DebuggerEnabled: %hhu\n\t- DebuggerPresent: %hhu\n\n",
				KernelDebuggerInfoEx.DebuggerAllowed, KernelDebuggerInfoEx.DebuggerEnabled, KernelDebuggerInfoEx.DebuggerPresent);
	}

	const UCHAR KdDebuggerEnabled = SharedUserData->KdDebuggerEnabled;
	Printf(L"SharedUserData->KdDebuggerEnabled: 0x%02hhX\n\n", KdDebuggerEnabled);

	if (RtlNtMajorVersion() > 6)
	{
		UCHAR KernelDebuggerFlags = 0;
		Status = NtQuerySystemInformation(SystemKernelDebuggerFlags,
											&KernelDebuggerFlags,
											sizeof(KernelDebuggerFlags),
											nullptr);
		if (!NT_SUCCESS(Status))
			Printf(L"SystemKernelDebuggerFlags: error %08lX\n\n", Status);
		else
			Printf(L"SystemKernelDebuggerFlags: 0x%02hhX\n\n", KernelDebuggerFlags);

		SYSTEM_CODEINTEGRITYPOLICY_INFORMATION CodeIntegrityPolicyInfo = { 0 };
		Status = NtQuerySystemInformation(SystemCodeIntegrityPolicyInformation,
											&CodeIntegrityPolicyInfo,
											sizeof(CodeIntegrityPolicyInfo),
											nullptr);
		if (!NT_SUCCESS(Status))
			Printf(L"SystemCodeIntegrityPolicyInformation: error %08lX\n\n", Status);
		else
			Printf(L"SystemCodeIntegrityPolicyInformation:\n\t- Options: 0x%04lX\n\t- HVCIOptions: 0x%04lX\n\n",
				CodeIntegrityPolicyInfo.Options, CodeIntegrityPolicyInfo.HVCIOptions);

		SYSTEM_ISOLATED_USER_MODE_INFORMATION IumInfo = { 0 };
		Status = NtQuerySystemInformation(SystemIsolatedUserModeInformation,
											&IumInfo,
											sizeof(IumInfo),
											nullptr);
		if (!NT_SUCCESS(Status))
			Printf(L"SystemIsolatedUserModeInformation: error %08lX\n\n", Status);
		else
			Printf(L"SystemIsolatedUserModeInformation:\n\t- SecureKernelRunning: %hhu\n\t- HvciEnabled: %hhu\n\t- HvciStrictMode: %hhu\n"
				"\t- DebugEnabled: %hhu\n\t- FirmwarePageProtection: %hhu\n\t- EncryptionKeyAvailable: %hhu\n\t- TrustletRunning: %hhu\n\t- HvciDisableAllowed: %hhu\n",
				IumInfo.SecureKernelRunning, IumInfo.HvciEnabled, IumInfo.HvciStrictMode, IumInfo.DebugEnabled, IumInfo.FirmwarePageProtection,
				IumInfo.EncryptionKeyAvailable, IumInfo.TrustletRunning, IumInfo.HvciDisableAllowed);
	}

	return Status;
}

//
// Reads one EFI variable as a single byte. Returns the NTSTATUS from the firmware call
// and, on success, the value and its attributes.
//
static
NTSTATUS
ReadFirmwareBoolean(
	_In_ PCWCHAR Name,
	_Out_ PUCHAR Value,
	_Out_ PULONG Attributes
	)
{
	UNICODE_STRING VariableName;
	RtlInitUnicodeString(&VariableName, Name);

	ULONG Size = sizeof(*Value);
	*Value = 0;
	NTSTATUS Status = NtQuerySystemEnvironmentValueEx(&VariableName,
													const_cast<LPGUID>(&gEfiGlobalVariableGuid),
													Value,
													&Size,
													Attributes);
	if (NT_SUCCESS(Status) && Size != sizeof(*Value))
		return STATUS_BUFFER_TOO_SMALL;

	return Status;
}

//
// Reads UEFISecureBootEnabled directly instead of through RtlQueryRegistryValues: the DIRECT query
// copies whatever type it finds into the caller's buffer, so a REG_SZ there would be written as a
// UNICODE_STRING over a ULONG. Opening the key and checking the type is explicit about it.
//
static
NTSTATUS
ReadSecureBootRegistryState(
	_Out_ PULONG Value
	)
{
	*Value = 0;

	UNICODE_STRING KeyPath;
	RtlInitUnicodeString(&KeyPath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\SecureBoot\\State");

	OBJECT_ATTRIBUTES Attributes;
	InitializeObjectAttributes(&Attributes, &KeyPath, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

	HANDLE Key = nullptr;
	NTSTATUS Status = NtOpenKey(&Key, KEY_QUERY_VALUE, &Attributes);
	if (!NT_SUCCESS(Status))
		return Status;

	UNICODE_STRING ValueName;
	RtlInitUnicodeString(&ValueName, L"UEFISecureBootEnabled");

	union
	{
		KEY_VALUE_PARTIAL_INFORMATION Information;
		UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
	} Data = {};
	ULONG ResultLength = 0;
	Status = NtQueryValueKey(Key,
							&ValueName,
							KeyValuePartialInformation,
							&Data,
							sizeof(Data),
							&ResultLength);
	NtClose(Key);
	if (!NT_SUCCESS(Status))
		return Status;

	if (Data.Information.Type != REG_DWORD || Data.Information.DataLength != sizeof(ULONG))
		return STATUS_OBJECT_TYPE_MISMATCH;

	RtlCopyMemory(Value, Data.Information.Data, sizeof(ULONG));
	return STATUS_SUCCESS;
}

// Generic single-DWORD registry read; the CI state key is volatile and may be absent.
static
NTSTATUS
ReadRegistryDword(
	_In_ PCWSTR KeyPath,
	_In_ PCWSTR ValueName,
	_Out_ PULONG Value
	)
{
	*Value = 0;

	UNICODE_STRING Path;
	RtlInitUnicodeString(&Path, KeyPath);
	OBJECT_ATTRIBUTES Attributes;
	InitializeObjectAttributes(&Attributes, &Path, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

	HANDLE Key = nullptr;
	NTSTATUS Status = NtOpenKey(&Key, KEY_QUERY_VALUE, &Attributes);
	if (!NT_SUCCESS(Status))
		return Status;

	UNICODE_STRING Name;
	RtlInitUnicodeString(&Name, ValueName);

	union
	{
		KEY_VALUE_PARTIAL_INFORMATION Information;
		UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
	} Data = {};
	ULONG ResultLength = 0;
	Status = NtQueryValueKey(Key, &Name, KeyValuePartialInformation, &Data, sizeof(Data), &ResultLength);
	NtClose(Key);
	if (!NT_SUCCESS(Status))
		return Status;
	if (Data.Information.Type != REG_DWORD || Data.Information.DataLength != sizeof(ULONG))
		return STATUS_OBJECT_TYPE_MISMATCH;

	*Value = *reinterpret_cast<PULONG>(Data.Information.Data);
	return STATUS_SUCCESS;
}

static
VOID
PrintCiOptions(
	_In_ ULONG Options
	)
{
	static const struct { ULONG Mask; PCWCHAR Name; } Bits[] = {
		{ 0x00000001, L"ENABLED" }, { 0x00000002, L"TESTSIGN" },
		{ 0x00000004, L"UMCI_ENABLED" }, { 0x00000008, L"UMCI_AUDIT" },
		{ 0x00000400, L"HVCI_KMCI_ENABLED" }, { 0x00000800, L"HVCI_KMCI_AUDIT" },
		{ 0x00001000, L"HVCI_KMCI_STRICT" }, { 0x00002000, L"HVCI_IUM_ENABLED" },
		{ 0x00004000, L"WHQL_ENFORCEMENT" }, { 0x00008000, L"WHQL_AUDIT" },
	};
	for (ULONG i = 0; i < ARRAYSIZE(Bits); ++i)
	{
		if ((Options & Bits[i].Mask) != 0)
			Printf(L"%ls ", Bits[i].Name);
	}
}

//
// Prints every surface that reports VBS/HVCI state, so a faked state can be compared against a
// real one line by line. See docs/vbs.md.
//
NTSTATUS
ReportVbsState(
	)
{
	SYSTEM_CODEINTEGRITY_INFORMATION Ci = { sizeof(SYSTEM_CODEINTEGRITY_INFORMATION) };
	NTSTATUS Status = NtQuerySystemInformation(SystemCodeIntegrityInformation, &Ci, sizeof(Ci), nullptr);
	if (NT_SUCCESS(Status))
	{
		Printf(L"SystemCodeIntegrityInformation:      0x%08lX [", Ci.CodeIntegrityOptions);
		PrintCiOptions(Ci.CodeIntegrityOptions);
		Printf(L"]\n");
	}
	else
	{
		Printf(L"SystemCodeIntegrityInformation:      error 0x%08lX\n", Status);
	}

	SYSTEM_CODEINTEGRITYPOLICY_INFORMATION Policy = {};
	Status = NtQuerySystemInformation(SystemCodeIntegrityPolicyInformation, &Policy, sizeof(Policy), nullptr);
	if (NT_SUCCESS(Status))
		Printf(L"SystemCodeIntegrityPolicyInformation: options 0x%08lX, hvci 0x%08lX\n", Policy.Options, Policy.HVCIOptions);
	else
		Printf(L"SystemCodeIntegrityPolicyInformation: error 0x%08lX\n", Status);

	SYSTEM_ISOLATED_USER_MODE_INFORMATION Ium = {};
	Status = NtQuerySystemInformation(SystemIsolatedUserModeInformation, &Ium, sizeof(Ium), nullptr);
	if (NT_SUCCESS(Status))
	{
		Printf(L"SystemIsolatedUserModeInformation:   secure kernel %u, hvci %u, strict %u, fw page %u, key %u\n",
			Ium.SecureKernelRunning, Ium.HvciEnabled, Ium.HvciStrictMode, Ium.FirmwarePageProtection, Ium.EncryptionKeyAvailable);
	}
	else
	{
		Printf(L"SystemIsolatedUserModeInformation:   error 0x%08lX\n", Status);
	}

	ULONG HvciState = ULONG_MAX;
	if (NT_SUCCESS(ReadRegistryDword(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\CI\\State",
									L"HVCIEnabled", &HvciState)))
		Printf(L"CI\\State\\HVCIEnabled:               %lu\n", HvciState);
	else
		Printf(L"CI\\State\\HVCIEnabled:               (absent)\n");

	UNICODE_STRING PolicyName;
	RtlInitUnicodeString(&PolicyName, L"VbsPolicyDisabled");
	UCHAR PolicyValue = 0;
	ULONG PolicySize = sizeof(PolicyValue);
	ULONG PolicyAttributes = 0;
	Status = NtQuerySystemEnvironmentValueEx(&PolicyName,
											const_cast<LPGUID>(&gMicrosoftVendorGuid),
											&PolicyValue,
											&PolicySize,
											&PolicyAttributes);
	if (NT_SUCCESS(Status))
		Printf(L"EFI VbsPolicyDisabled:               %u (visible!)\n", PolicyValue);
	else
		Printf(L"EFI VbsPolicyDisabled:               error 0x%08lX\n", Status);

	EFIMIRAGE_STATE_DATA State = {};
	UNICODE_STRING StateName;
	RtlInitUnicodeString(&StateName, EFIMIRAGE_STATE_VARIABLE_NAME);
	ULONG StateSize = sizeof(State);
	ULONG StateAttributes = 0;
	Status = NtQuerySystemEnvironmentValueEx(&StateName,
											const_cast<LPGUID>(&gEfiMirageStateVariableGuid),
											&State,
											&StateSize,
											&StateAttributes);
	if (NT_SUCCESS(Status) && StateSize == sizeof(State) && State.Magic == EFIMIRAGE_STATE_MAGIC)
		Printf(L"EfiMirage state:                     flags 0x%lX (fake VBS %u)\n",
			State.Flags, (State.Flags & EFIMIRAGE_STATE_FLAG_FAKE_VBS) != 0);
	else
		Printf(L"EfiMirage state:                     not present\n");

	return STATUS_SUCCESS;
}

//
// Puts the registry back into "VBS and memory integrity configured" state. Windows clears these
// keys itself when VBS fails to start (which it does whenever the driver keeps VbsPolicyDisabled
// set), so they have to be restored at runtime. The CI state key is what memory integrity
// actually reports while it runs; creating it here is what a real HVCI boot does.
//
NTSTATUS
ApplyVbsState(
	)
{
	CONST PWCHAR DeviceGuard = const_cast<PWCHAR>(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\DeviceGuard");
	CONST PWCHAR Scenario = const_cast<PWCHAR>(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity");
	CONST PWCHAR CiState = const_cast<PWCHAR>(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\CI\\State");

	struct
	{
		PCWSTR Path;
		PCWSTR Value;
		ULONG Data;
	} Settings[] =
	{
		{ DeviceGuard, L"EnableVirtualizationBasedSecurity", 1 },
		{ DeviceGuard, L"RequirePlatformSecurityFeatures", 1 },
		{ DeviceGuard, L"Locked", 0 },
		{ Scenario,    L"Enabled", 1 },
		{ Scenario,    L"Locked", 0 },
		{ CiState,     L"HVCIEnabled", 1 },
	};

	// The kernel-side value a real memory-integrity machine carries; -V is what a logon task runs.
	CONST NTSTATUS CiStatus = SetCiOptionsHvciBits();
	Printf(L"  g_CiOptions HVCI bits: %ls\n", NT_SUCCESS(CiStatus) ? L"ok" : L"failed");

	NTSTATUS FirstError = STATUS_SUCCESS;
	for (ULONG i = 0; i < ARRAYSIZE(Settings); ++i)
	{
		CONST NTSTATUS Status = RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE,
													Settings[i].Path,
													Settings[i].Value,
													REG_DWORD,
													&Settings[i].Data,
													sizeof(Settings[i].Data));
		if (!NT_SUCCESS(Status) && NT_SUCCESS(FirstError))
			FirstError = Status;
		Printf(L"  %ls = %lu: %ls\n", Settings[i].Value, Settings[i].Data, NT_SUCCESS(Status) ? L"ok" : L"failed");
	}

	return FirstError;
}

//
// Reports what Windows sees of the Secure Boot state from every source an application can
// practically use. With EfiMirage's Secure Boot emulation active these all agree on "enabled";
// on a machine with Secure Boot genuinely off they agree on "disabled".
//
NTSTATUS
ReportSecureBootState(
	)
{
	UCHAR SecureBootValue = 0, SetupModeValue = 0;
	ULONG SecureBootAttributes = 0, SetupModeAttributes = 0;
	const NTSTATUS SecureBootStatus = ReadFirmwareBoolean(EFI_SECURE_BOOT_MODE_NAME, &SecureBootValue, &SecureBootAttributes);
	const NTSTATUS SetupModeStatus = ReadFirmwareBoolean(EFI_SETUP_MODE_NAME, &SetupModeValue, &SetupModeAttributes);

	SYSTEM_SECUREBOOT_INFORMATION SecureBootInfo = {};
	NTSTATUS SysInfoStatus = NtQuerySystemInformation(SystemSecureBootInformation,
													&SecureBootInfo,
													sizeof(SecureBootInfo),
													nullptr);

	ULONG RegistryValue = ULONG_MAX;
	const NTSTATUS RegistryStatus = ReadSecureBootRegistryState(&RegistryValue);

	Printf(L"Secure Boot state as observed from user mode:\n");

	if (NT_SUCCESS(SecureBootStatus))
		Printf(L"  EFI variable SecureBoot:          %hhu (attributes 0x%lX)\n", SecureBootValue, SecureBootAttributes);
	else
		Printf(L"  EFI variable SecureBoot:          error 0x%08lX\n", SecureBootStatus);

	if (NT_SUCCESS(SetupModeStatus))
		Printf(L"  EFI variable SetupMode:           %hhu (attributes 0x%lX)\n", SetupModeValue, SetupModeAttributes);
	else
		Printf(L"  EFI variable SetupMode:           error 0x%08lX\n", SetupModeStatus);

	if (NT_SUCCESS(SysInfoStatus))
		Printf(L"  SystemSecureBootInformation:      enabled %hhu, capable %hhu\n",
			SecureBootInfo.SecureBootEnabled, SecureBootInfo.SecureBootCapable);
	else
		Printf(L"  SystemSecureBootInformation:      error 0x%08lX\n", SysInfoStatus);

	if (NT_SUCCESS(RegistryStatus))
		Printf(L"  Registry UEFISecureBootEnabled:   %lu\n", RegistryValue);
	else
		Printf(L"  Registry UEFISecureBootEnabled:   error 0x%08lX\n", RegistryStatus);

	// An application that requires Secure Boot consults at least one of these. All of them
	// agreeing on "enabled" with SetupMode off is the state a genuine Secure Boot boot has.
	const bool ReportedlyEnabled = NT_SUCCESS(SecureBootStatus) && SecureBootValue != 0 &&
		NT_SUCCESS(SetupModeStatus) && SetupModeValue == 0 &&
		NT_SUCCESS(SysInfoStatus) && SecureBootInfo.SecureBootEnabled != 0 &&
		NT_SUCCESS(RegistryStatus) && RegistryValue == 1;

	Printf(L"\n  Verdict: %ls\n", ReportedlyEnabled
		? L"Secure Boot appears ENABLED from all sources."
		: L"Secure Boot is not consistently reported as enabled; check the sources above.");

	return ReportedlyEnabled ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
