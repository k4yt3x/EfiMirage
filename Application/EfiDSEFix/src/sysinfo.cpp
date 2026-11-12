#include "EfiDSEFix.h"
#include "EfiCompat.h"
#include <ntstatus.h>
#include <Guid/GlobalVariable.h>

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

static
NTSTATUS
ReadSecureBootRegistryState(
	_Out_ PULONG Value
	)
{
	*Value = ULONG_MAX;

	RTL_QUERY_REGISTRY_TABLE Table[2] = {};
	Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
	Table[0].Name = const_cast<PWSTR>(L"UEFISecureBootEnabled");
	Table[0].EntryContext = Value;
	Table[0].DefaultType = REG_DWORD;
	Table[0].DefaultData = Value;
	Table[0].DefaultLength = sizeof(*Value);

	return RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
								L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\SecureBoot\\State",
								Table,
								nullptr,
								nullptr);
}

//
// Reports what Windows sees of the Secure Boot state from every source an application can
// practically use. With EfiGuard's Secure Boot emulation active these all agree on "enabled";
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
		NT_SUCCESS(RegistryStatus) && RegistryValue != 0;

	Printf(L"\n  Verdict: %ls\n", ReportedlyEnabled
		? L"Secure Boot appears ENABLED from all sources."
		: L"Secure Boot is not consistently reported as enabled; check the sources above.");

	return ReportedlyEnabled ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
