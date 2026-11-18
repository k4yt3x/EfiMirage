#ifndef __EFIMIRAGE_GUID_H__
#define __EFIMIRAGE_GUID_H__

#include <Guid/GlobalVariable.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// EfiMirage Bootkit Protocol GUID
//
#define EFI_EFIMIRAGE_DRIVER_PROTOCOL_GUID \
	{ \
	0x51e4785b, 0xb1e4, 0x4fda, { 0xaf, 0x5f, 0x94, 0x2e, 0xc0, 0x15, 0xf1, 0x7 } \
	}

//
// Type of Driver Signature Enforcement bypass to use
//
typedef enum _EFIMIRAGE_DSE_BYPASS_TYPE {
	//
	// Do not disable DSE.
	//
	DSE_DISABLE_NONE = 0,

	//
	// Prevent DSE initialization at boot by patching SepInitializeCodeIntegrity.
	// DSE will remain disabled until system reboot.
	//
	// Note: This can be trivially detected. If this is not a problem for you,
	// this is the most convenient option.
	//
	DSE_DISABLE_AT_BOOT = 1,

	//
	// Hook the EFI SetVariable() runtime service to provide a stealth method for writing
	// to any kernel address. This is therefore not a true DSE bypass but simply a backdoor.
	// The most obvious use however is to set g_CiOptions/g_CiEnabled to 0 to load any driver.
	//
	// This is the default DSE bypass setting.
	//
	DSE_DISABLE_SETVARIABLE_HOOK = 2
} EFIMIRAGE_DSE_BYPASS_TYPE;


//
// Kernel read/write backdoor struct, used in combination with DSE bypass type DSE_DISABLE_SETVARIABLE_HOOK.
// For scalar values, use one of the Byte through Qword fields, set its size in Size, and set ReadOnly to FALSE.
// For writes, the field that was used to supply the data will contain the original value on return.
//
// To perform a memcpy, set UserBuffer to a pointer-aligned buffer, Size to the size of the buffer, and
// ReadOnly to FALSE. There is no SEH in UEFI for buffer probing, so it is the caller's responsibility that
// the address is valid and correctly aligned. No backup of the original buffer will be made because this
// would require memory allocation at runtime. If you wish to obtain the contents of the current data at
// KernelAddress, call the backdoor twice with the first call having ReadOnly = TRUE (or use a scalar size).
//
// If ReadOnly is TRUE, no writes to kernel memory will be performed. Instead either
// (1) one of the Byte through Qword fields (depending on size) will contain the value at KernelAddress, or
// (2) the memcpy performed will be in the opposite direction, i.e. from KernelAddress to UserBuffer.
//
// Keep the legacy wire name so existing EfiGuard clients still address the same interface.
#define EFIMIRAGE_BACKDOOR_VARIABLE_NAME						L"roodkcaBdrauGifE"
#define EFIMIRAGE_BACKDOOR_VARIABLE_GUID						&gEfiGlobalVariableGuid
#define EFIMIRAGE_BACKDOOR_VARIABLE_ATTRIBUTES				(EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define EFIMIRAGE_BACKDOOR_VARIABLE_DATASIZE					sizeof(EFIMIRAGE_BACKDOOR_DATA)

#define EFIMIRAGE_BACKDOOR_COOKIE_VALUE						(0xDEADC0DE)

typedef struct _EFIMIRAGE_BACKDOOR_DATA {

	UINTN CookieValue; // Currently must be EFIMIRAGE_BACKDOOR_COOKIE_VALUE
	VOID* KernelAddress;

	union {
		struct {
			UINT64 Byte : 8;
			UINT64 Word : 16;
			UINT64 Dword : 32;
			UINT64 Spare : 8;
		} s;

		UINT64 Qword;
		VOID* UserBuffer;
	} u;

	UINT32 Size;
	BOOLEAN ReadOnly;
} EFIMIRAGE_BACKDOOR_DATA;


//
// Configuration data version. Must equal the value below or Configure() is refused.
//
#define EFIMIRAGE_CONFIGURATION_DATA_VERSION 2

//
// Reported state left behind for the Windows-side client, so it can tell an emulated VBS state
// from a real one. Boot-service + runtime, not non-volatile: it lives only for this boot.
//
#define EFIMIRAGE_STATE_VARIABLE_NAME L"MirageState"
#define EFIMIRAGE_STATE_VARIABLE_GUID \
	{ 0x6b1f3a52, 0x8c4d, 0x4e77, { 0x9a, 0x2b, 0x51, 0x13, 0xc8, 0x6f, 0x2d, 0x40 } }
#define EFIMIRAGE_STATE_MAGIC 0x4547524D  // 'MRGE'
#define EFIMIRAGE_STATE_VERSION 1

#define EFIMIRAGE_STATE_FLAG_FAKE_VBS 0x1
#define EFIMIRAGE_STATE_FLAG_FAKE_SECURE_BOOT 0x2

typedef struct _EFIMIRAGE_STATE_DATA {
	UINT32 Magic;
	UINT32 Version;
	UINT32 Flags;
} EFIMIRAGE_STATE_DATA;


//
// Main driver configuration data. This can be optionally sent to the driver using the Configure() pointer in the protocol.
//
typedef struct _EFIMIRAGE_CONFIGURATION_DATA {
	//
	// Must be EFIMIRAGE_CONFIGURATION_DATA_VERSION.
	//
	// First on purpose: a caller built against an older, shorter struct reads its own bytes
	// here rather than reading past its buffer, and a zero-initialized struct is refused.
	//
	UINT32 Version;

	//
	// Type of Driver Signature Enforcement bypass to use.
	// Default: DSE_DISABLE_SETVARIABLE_HOOK
	//
	EFIMIRAGE_DSE_BYPASS_TYPE DseBypassMethod;

	//
	// Whether to wait for a keypress at the end of each patch stage, regardless of success or failure.
	// Recommended for debugging purposes only.
	// Default: FALSE
	//
	BOOLEAN WaitForKeyPress;

	//
	// Whether to make the firmware's EFI Secure Boot state look enabled to everything that reads it.
	// SecureBoot is reported as 1 and SetupMode as 0 for both boot services and runtime callers, so
	// winload, the kernel and user mode all observe a consistent "Secure Boot on, user mode" state
	// regardless of the actual firmware configuration. See docs/secureboot.md for the limits of this.
	// Default: TRUE
	//
	BOOLEAN FakeSecureBoot;

	//
	// Whether to make VBS and HVCI look enabled and running to everything that asks the kernel.
	// 0 = off, 1 = HVCI enforced, 2 = HVCI audit. The kernel patches only change what
	// NtQuerySystemInformation reports; VBS itself stays off so the SetVariable backdoor keeps
	// working. See docs/vbs.md for the surfaces this covers and the ones it cannot.
	// Default: 0
	//
	UINT8 FakeVbs;

	UINT8 Reserved[3];
} EFIMIRAGE_CONFIGURATION_DATA;


//
// Sends configuration data to the driver.
//
typedef
EFI_STATUS
(EFIAPI*
EFIMIRAGE_CONFIGURE)(
	IN CONST EFIMIRAGE_CONFIGURATION_DATA* ConfigurationData
	);


//
// The EfiMirage bootkit driver protocol.
//
typedef struct _EFIMIRAGE_DRIVER_PROTOCOL {
	EFIMIRAGE_CONFIGURE Configure;
} EFIMIRAGE_DRIVER_PROTOCOL;


extern EFI_GUID gEfiMirageDriverProtocolGuid;

#ifdef __cplusplus
}
#endif

#endif
