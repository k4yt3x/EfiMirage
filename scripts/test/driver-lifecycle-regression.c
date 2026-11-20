// Whole-driver host fixture: firmware services, image classification and patching are mocks.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NULL
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "EfiMirageDxe.c"
#pragma GCC diagnostic pop

static unsigned Failures;
#define CHECK(Expression) do { if (!(Expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #Expression); ++Failures; \
} } while (0)

EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiDriverSupportedEfiVersionProtocolGuid = EFI_DRIVER_SUPPORTED_EFI_VERSION_PROTOCOL_GUID;
EFI_GUID gEfiMirageDriverProtocolGuid = EFI_EFIMIRAGE_DRIVER_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInputExProtocolGuid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
EFI_GUID gEfiShellProtocolGuid = EFI_SHELL_PROTOCOL_GUID;
EFI_GUID gEfiEventExitBootServicesGuid = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
EFI_GUID gEfiEventVirtualAddressChangeGuid = EFI_EVENT_GROUP_VIRTUAL_ADDRESS_CHANGE;
EFI_GUID gEfiGlobalVariableGuid = EFI_GLOBAL_VARIABLE;
EFI_HANDLE gImageHandle = (EFI_HANDLE)1;
static EFI_BOOT_SERVICES BootServices;
static EFI_RUNTIME_SERVICES RuntimeServices;
static EFI_SYSTEM_TABLE SystemTable;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;
EFI_BOOT_SERVICES* gBS = &BootServices;
EFI_RUNTIME_SERVICES* gRT = &RuntimeServices;
EFI_SYSTEM_TABLE* gST = &SystemTable;
KERNEL_PATCH_INFORMATION gKernelPatchInfo;

static struct {
	EFI_EVENT_NOTIFY Notify;
	BOOLEAN Closed;
} ExitEvent, VirtualEvent;
static UINTN Uninstalls, SetVariableCalls, BootPatches, Classifications, HandleQueries;
static UINT32 StateFlags;
static BOOLEAN SecureBootInstalled, FailVirtualEvent, FailStateDelete, FailStateWrite;
static UINTN SecureBootUninstalls, Conversions;
static jmp_buf BootExit, ResetExit;
static BOOLEAN ResetCalled;
static CHAR16 ExitMessage[] = L"mock image exit";
static CHAR16 ProbeName[] = EFIMIRAGE_BACKDOOR_VARIABLE_NAME;
static UINTN ExpectedAttributes = 0x1234;

typedef enum { Return, ExitBoot, NestedReturn, NestedExit, UnloadInFlight } ACTION;
typedef struct MOCK_IMAGE {
	EFI_LOADED_IMAGE_PROTOCOL Loaded;
	INPUT_FILETYPE Type;
	EFI_STATUS ProtocolStatus, StartStatus;
	BOOLEAN NullProtocol, Eligible;
	ACTION Action;
	struct MOCK_IMAGE* Child;
	UINTN Starts;
} MOCK_IMAGE;
static MOCK_IMAGE Windows, Other;
static MOCK_IMAGE* ImageToLoad;

VOID* EFIAPI CopyMem(VOID* Destination, CONST VOID* Source, UINTN Length)
{
	return memmove(Destination, Source, Length);
}

VOID* EFIAPI SetMem64(VOID* Buffer, UINTN Length, UINT64 Value)
{
	CHECK(Value == 0);
	return memset(Buffer, 0, Length);
}

BOOLEAN EFIAPI CompareGuid(CONST GUID* Left, CONST GUID* Right)
{
	return memcmp(Left, Right, sizeof(*Left)) == 0;
}

INTN EFIAPI StrnCmp(CONST CHAR16* Left, CONST CHAR16* Right, UINTN Length)
{
	for (UINTN i = 0; i < Length; ++i)
	{
		if (Left[i] != Right[i])
			return (INTN)Left[i] - Right[i];
		if (Left[i] == 0)
			break;
	}
	return 0;
}

UINTN EFIAPI AsmReadCr0(VOID) { return 0; }
UINTN EFIAPI AsmWriteCr0(UINTN Value) { CHECK(FALSE); return Value; }
VOID* EFIAPI InterlockedCompareExchangePointer(VOID* volatile* Value, VOID* Compare, VOID* Exchange)
{
	VOID* Previous = *Value;
	CHECK(Previous == Compare);
	*Value = Exchange;
	return Previous;
}

UINTN EFIAPI Print(CONST CHAR16* Format, ...) { (void)Format; return 0; }
INT32 EFIAPI SetConsoleTextColour(UINTN Colour, BOOLEAN Clear)
{
	(void)Colour;
	(void)Clear;
	return EFI_LIGHTGRAY;
}
EFI_STATUS EFIAPI RtlSleep(UINTN Milliseconds) { (void)Milliseconds; return EFI_SUCCESS; }
EFI_STATUS EFIAPI RtlStall(UINTN Milliseconds) { (void)Milliseconds; CHECK(FALSE); return EFI_SUCCESS; }
VOID EFIAPI PrintLoadedImageInfo(CONST EFI_LOADED_IMAGE* Image) { (void)Image; }
VOID EFIAPI PrintKernelPatchInfo(VOID) { CHECK(FALSE); }
BOOLEAN EFIAPI WaitForKey(VOID) { CHECK(FALSE); return TRUE; }
VOID EFIAPI FreePool(VOID* Buffer) { (void)Buffer; CHECK(FALSE); }
CONST CHAR16* EFIAPI StriStr(CONST CHAR16* Text, CONST CHAR16* Search)
{
	(void)Text;
	(void)Search;
	CHECK(FALSE);
	return NULL;
}
CHAR16* EFIAPI ConvertDevicePathToText(CONST EFI_DEVICE_PATH_PROTOCOL* Path,
	BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts)
{
	(void)Path;
	(void)DisplayOnly;
	(void)AllowShortcuts;
	return NULL;
}

// The deletion probe never supplies data or an address. Memory operations must remain unreachable.
VOID* EFIAPI CopyWpMem(VOID* Destination, CONST VOID* Source, UINTN Length)
{
	(void)Destination;
	(void)Source;
	(void)Length;
	fputs("unexpected memory-operation branch\n", stderr);
	exit(1);
}

INPUT_FILETYPE EFIAPI GetInputFileType(CONST UINT8* ImageBase, UINTN ImageSize)
{
	CHECK(ImageSize == sizeof(MOCK_IMAGE));
	CHECK(ImageBase == (CONST UINT8*)&Windows || ImageBase == (CONST UINT8*)&Other);
	++Classifications;
	return ((CONST MOCK_IMAGE*)ImageBase)->Type;
}
EFI_STATUS EFIAPI PatchBootManager(INPUT_FILETYPE Type, CONST VOID* Base, UINTN Size)
{
	CHECK(Type == BootmgfwEfi && Base == &Windows && Size == sizeof(Windows));
	++BootPatches;
	return EFI_SUCCESS;
}

EFI_STATUS EFIAPI InstallSecureBootHook(VOID) { SecureBootInstalled = TRUE; return EFI_SUCCESS; }
VOID EFIAPI UninstallSecureBootHook(VOID) { SecureBootInstalled = FALSE; ++SecureBootUninstalls; }
VOID EFIAPI SecureBootConvertRuntimePointers(VOID) { }

static EFI_TPL EFIAPI MockRaiseTpl(EFI_TPL Tpl) { CHECK(Tpl == TPL_HIGH_LEVEL); return TPL_APPLICATION; }
static VOID EFIAPI MockRestoreTpl(EFI_TPL Tpl) { CHECK(Tpl == TPL_APPLICATION); }
static EFI_STATUS EFIAPI MockCrc(VOID* Data, UINTN Size, UINT32* Crc)
{
	CHECK(Data == &BootServices || Data == &RuntimeServices);
	CHECK(Size == ((EFI_TABLE_HEADER*)Data)->HeaderSize);
	*Crc = 0x12345678;
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockAttribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, UINTN Attribute)
{
	(void)Attribute;
	CHECK(This == &Console);
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockCursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, BOOLEAN Visible)
{
	CHECK(This == &Console && !Visible);
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockLocate(EFI_GUID* Protocol, VOID* Registration, VOID** Interface)
{
	(void)Protocol;
	(void)Registration;
	*Interface = NULL;
	return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI MockHandle(EFI_HANDLE Handle, EFI_GUID* Protocol, VOID** Interface)
{
	*Interface = NULL;
	if (Protocol == &gEfiSimpleTextInputExProtocolGuid)
		return EFI_UNSUPPORTED;
	CHECK(Protocol == &gEfiLoadedImageProtocolGuid);
	CHECK(Handle == &Windows || Handle == &Other || Handle == gImageHandle);
	if (Handle == gImageHandle)
	{
		*Interface = &Other.Loaded;
		return EFI_SUCCESS;
	}
	++HandleQueries;
	MOCK_IMAGE* Image = Handle;
	// A failure can leave an output value behind; the status must still take precedence.
	*Interface = Image->NullProtocol ? NULL : &Image->Loaded;
	return Image->ProtocolStatus;
}
static EFI_STATUS EFIAPI MockOpen(EFI_HANDLE Handle, EFI_GUID* Protocol, VOID** Interface,
	EFI_HANDLE Agent, EFI_HANDLE Controller, UINT32 Attributes)
{
	CHECK(Agent == gImageHandle && Controller == NULL && Attributes == EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	return MockHandle(Handle, Protocol, Interface);
}
static EFI_STATUS EFIAPI MockInstallMultiple(EFI_HANDLE* Handle, ...)
{
	CHECK(*Handle == gImageHandle);
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockInstall(EFI_HANDLE* Handle, EFI_GUID* Protocol,
	EFI_INTERFACE_TYPE Type, VOID* Interface)
{
	CHECK(*Handle == gImageHandle && Protocol == &gEfiMirageDriverProtocolGuid);
	CHECK(Type == EFI_NATIVE_INTERFACE && Interface == &gEfiMirageDriverProtocol);
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockUninstall(EFI_HANDLE Handle, ...)
{
	CHECK(Handle == gImageHandle);
	++Uninstalls;
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockCreateEvent(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify,
	CONST VOID* Context, CONST EFI_GUID* Group, EFI_EVENT* Event)
{
	CHECK(Type == EVT_NOTIFY_SIGNAL && Tpl == TPL_NOTIFY && Context == NULL);
	if (Group == &gEfiEventExitBootServicesGuid)
	{
		ExitEvent.Notify = Notify;
		*Event = &ExitEvent;
	}
	else
	{
		CHECK(Group == &gEfiEventVirtualAddressChangeGuid);
		if (FailVirtualEvent)
			return EFI_OUT_OF_RESOURCES;
		VirtualEvent.Notify = Notify;
		*Event = &VirtualEvent;
	}
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockCloseEvent(EFI_EVENT Event)
{
	if (Event == &ExitEvent)
	{
		CHECK(!ExitEvent.Closed);
		ExitEvent.Closed = TRUE;
	}
	else
	{
		CHECK(Event == &VirtualEvent && !VirtualEvent.Closed);
		VirtualEvent.Closed = TRUE;
	}
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockConvertPointer(UINTN Disposition, VOID** Address)
{
	CHECK(Disposition == 0 && Address != NULL && *Address != NULL);
	++Conversions;
	return EFI_SUCCESS;
}
static VOID EFIAPI MockReset(EFI_RESET_TYPE Type, EFI_STATUS Status, UINTN Size, VOID* Data)
{
	CHECK(Type == EfiResetCold && Status == EFI_WRITE_PROTECTED && Size == 0 && Data == NULL);
	ResetCalled = TRUE;
	longjmp(ResetExit, 1);
}
static EFI_STATUS EFIAPI MockGetVariable(CHAR16* Name, EFI_GUID* Guid, UINT32* Attributes,
	UINTN* Size, VOID* Data)
{
	CONST CHAR16 StateName[] = EFIMIRAGE_STATE_VARIABLE_NAME;
	CONST EFI_GUID StateGuid = EFIMIRAGE_STATE_VARIABLE_GUID;
	CHECK(StrnCmp(Name, StateName, sizeof(StateName) / sizeof(CHAR16)) == 0);
	CHECK(CompareGuid(Guid, &StateGuid) && Size != NULL);
	if (StateFlags == 0)
		return EFI_NOT_FOUND;
	if (*Size < sizeof(EFIMIRAGE_STATE_DATA))
	{
		*Size = sizeof(EFIMIRAGE_STATE_DATA);
		return EFI_BUFFER_TOO_SMALL;
	}
	CHECK(Data != NULL);
	if (Attributes != NULL)
		*Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
	*Size = sizeof(EFIMIRAGE_STATE_DATA);
	*(EFIMIRAGE_STATE_DATA*)Data = (EFIMIRAGE_STATE_DATA){
		EFIMIRAGE_STATE_MAGIC, EFIMIRAGE_STATE_VERSION, StateFlags
	};
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockSetVariable(CHAR16* Name, EFI_GUID* Guid, UINT32 Attributes,
	UINTN Size, VOID* Data)
{
	CONST CHAR16 StateName[] = EFIMIRAGE_STATE_VARIABLE_NAME;
	CONST EFI_GUID StateGuid = EFIMIRAGE_STATE_VARIABLE_GUID;
	if (Name != NULL && StrnCmp(Name, StateName, sizeof(StateName) / sizeof(CHAR16)) == 0)
	{
		CHECK(CompareGuid(Guid, &StateGuid));
		if (Attributes == 0 && Size == 0 && Data == NULL)
		{
			if (FailStateDelete)
				return EFI_WRITE_PROTECTED;
			StateFlags = 0;
			return EFI_SUCCESS;
		}
		CHECK(Attributes == (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS));
		CHECK(Size == sizeof(EFIMIRAGE_STATE_DATA) && Data != NULL);
		if (FailStateWrite)
			return EFI_WRITE_PROTECTED;
		CONST EFIMIRAGE_STATE_DATA* State = Data;
		CHECK(State->Magic == EFIMIRAGE_STATE_MAGIC && State->Version == EFIMIRAGE_STATE_VERSION);
		StateFlags = State->Flags;
		return EFI_SUCCESS;
	}

	CHECK(Name == ProbeName && Guid == EFIMIRAGE_BACKDOOR_VARIABLE_GUID);
	CHECK(Attributes == ExpectedAttributes && Size == 0 && Data == NULL);
	++SetVariableCalls;
	return EFI_WRITE_PROTECTED;
}

static void Probe(BOOLEAN Eligible)
{
	// Exercise runtime eligibility without changing the StartImage execution scope.
	BOOLEAN Runtime = gEfiAtRuntime, Virtual = gEfiGoneVirtual;
	UINTN Calls = SetVariableCalls;
	gEfiAtRuntime = gEfiGoneVirtual = TRUE;
	EFI_STATUS Status = RuntimeServices.SetVariable(ProbeName,
		EFIMIRAGE_BACKDOOR_VARIABLE_GUID, ExpectedAttributes, 0, NULL);
	CHECK(Status == (Eligible ? EFI_SUCCESS : EFI_WRITE_PROTECTED));
	CHECK(SetVariableCalls == Calls + (Eligible ? 0 : 1));
	gEfiAtRuntime = Runtime;
	gEfiGoneVirtual = Virtual;
}

static void Start(MOCK_IMAGE* Image)
{
	UINTN Size = 777;
	CHAR16* Data = NULL;
	UINTN Starts = Image->Starts;
	CHECK(BootServices.StartImage(Image, &Size, &Data) == Image->StartStatus);
	CHECK(Image->Starts == Starts + 1);
	CHECK(Size == sizeof(ExitMessage) && Data == ExitMessage);
}

static EFI_STATUS EFIAPI MockStart(EFI_HANDLE Handle, UINTN* Size, CHAR16** Data)
{
	CHECK(Handle == &Windows || Handle == &Other);
	MOCK_IMAGE* Image = Handle;
	++Image->Starts;
	CHECK(Size != NULL && *Size == 777);
	if (Data != NULL)
	{
		CHECK(*Data == NULL);
		*Data = ExitMessage;
	}
	*Size = sizeof(ExitMessage);
	Probe(Image->Eligible);
	if (Image->Action == NestedReturn || Image->Action == NestedExit)
	{
		Start(Image->Child);
		Probe(Image->Eligible);
	}
	if (Image->Action == UnloadInFlight)
	{
		CHECK(gBootmgfwHandle == NULL);
		CHECK(EfiMirageUnload(gImageHandle) == EFI_ACCESS_DENIED);
		CHECK(Uninstalls == 0 && !ExitEvent.Closed && !VirtualEvent.Closed);
		CHECK(BootServices.StartImage == HookedStartImage && RuntimeServices.SetVariable == HookedSetVariable);
		CHECK(SecureBootInstalled);
		EFIMIRAGE_CONFIGURATION_DATA Config = gDriverConfig;
		Config.WaitForKeyPress = TRUE;
		CHECK(DriverConfigure(&Config) == EFI_SUCCESS);
		CHECK(gDriverConfig.WaitForKeyPress);
	}
	if (Image->Action == ExitBoot || Image->Action == NestedReturn)
	{
		ExitEvent.Notify(&ExitEvent, NULL);
		// Firmware never returns to this boot-services wrapper after successful EBS.
		longjmp(BootExit, 1);
	}
	return Image->StartStatus;
}
static EFI_STATUS EFIAPI MockLoad(BOOLEAN Policy, EFI_HANDLE Parent,
	EFI_DEVICE_PATH_PROTOCOL* Path, VOID* Buffer, UINTN Size, EFI_HANDLE* Handle)
{
	CHECK(Policy && Parent == gImageHandle && Path == NULL && Buffer == NULL && Size == 0);
	*Handle = ImageToLoad;
	return EFI_SUCCESS;
}

static void LoadWindows(void)
{
	ImageToLoad = &Windows;
	EFI_HANDLE Handle = NULL;
	CHECK(BootServices.LoadImage(TRUE, gImageHandle, NULL, NULL, 0, &Handle) == EFI_SUCCESS);
	CHECK(Handle == &Windows && gBootmgfwHandle == Handle && BootPatches == 1);
	CHECK(Windows.Loaded.ParentHandle == NULL);
	Probe(FALSE);
}

static void Initialize(EFIMIRAGE_DSE_BYPASS_TYPE Mode, BOOLEAN Fake)
{
	Windows.Type = BootmgfwEfi;
	Other.Type = Unknown;
	Windows.Loaded.ImageBase = &Windows;
	Other.Loaded.ImageBase = &Other;
	Windows.Loaded.ImageSize = Other.Loaded.ImageSize = sizeof(MOCK_IMAGE);
	Windows.Loaded.ParentHandle = gImageHandle;
	Windows.StartStatus = EFI_LOAD_ERROR;
	Other.StartStatus = EFI_ABORTED;
	BootServices.Hdr.HeaderSize = sizeof(BootServices);
	RuntimeServices.Hdr.HeaderSize = sizeof(RuntimeServices);
	BootServices.RaiseTPL = MockRaiseTpl;
	BootServices.RestoreTPL = MockRestoreTpl;
	BootServices.CalculateCrc32 = MockCrc;
	BootServices.LocateProtocol = MockLocate;
	BootServices.HandleProtocol = MockHandle;
	BootServices.OpenProtocol = MockOpen;
	BootServices.InstallMultipleProtocolInterfaces = MockInstallMultiple;
	BootServices.InstallProtocolInterface = MockInstall;
	BootServices.UninstallMultipleProtocolInterfaces = MockUninstall;
	BootServices.CreateEventEx = MockCreateEvent;
	BootServices.CloseEvent = MockCloseEvent;
	BootServices.LoadImage = MockLoad;
	BootServices.StartImage = MockStart;
	RuntimeServices.SetVariable = MockSetVariable;
	RuntimeServices.GetVariable = MockGetVariable;
	RuntimeServices.ConvertPointer = MockConvertPointer;
	RuntimeServices.ResetSystem = MockReset;
	SystemTable.ConOut = &Console;
	Console.SetAttribute = MockAttribute;
	Console.EnableCursor = MockCursor;
	EFI_STATUS Status = EfiMirageInitialize(gImageHandle, &SystemTable);
	CHECK(Status == (FailVirtualEvent ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS));
	if (FailVirtualEvent)
		return;
	CHECK(BootServices.LoadImage == HookedLoadImage && BootServices.StartImage == HookedStartImage);
	CHECK(RuntimeServices.SetVariable == HookedSetVariable && SecureBootInstalled);
	CHECK(ExitEvent.Notify == ExitBootServicesEvent && VirtualEvent.Notify == SetVirtualAddressMapEvent);
	EFIMIRAGE_CONFIGURATION_DATA Config = gDriverConfig;
	Config.DseBypassMethod = Mode;
	Config.FakeSecureBoot = Fake;
	CHECK(gEfiMirageDriverProtocol.Configure(&Config) == EFI_SUCCESS);
}

static void CheckUnloaded(void)
{
	CHECK(BootServices.StartImage == MockStart && BootServices.LoadImage == MockLoad);
	CHECK(RuntimeServices.SetVariable == MockSetVariable && !SecureBootInstalled);
	CHECK(Uninstalls == 1 && ExitEvent.Closed);
	CHECK(FailVirtualEvent || VirtualEvent.Closed);
	CHECK(gEfiExitBootServicesEvent == NULL && gEfiVirtualNotifyEvent == NULL);
}

static void CheckBootExit(MOCK_IMAGE* Image, BOOLEAN Keep, BOOLEAN Fake)
{
	if (setjmp(BootExit) == 0)
	{
		if (Image != NULL)
			Start(Image);
		else
			ExitEvent.Notify(&ExitEvent, NULL);
		CHECK(Image == NULL);
	}
	CHECK(gBS == NULL && gEfiAtRuntime && ExitEvent.Closed);
	CHECK(RuntimeServices.SetVariable == (Keep ? HookedSetVariable : MockSetVariable));
	CHECK(SecureBootInstalled == Fake && SecureBootUninstalls == (Fake ? 0 : 1));
	CHECK(Uninstalls == (Keep || Fake ? 0 : 1));
	CHECK(VirtualEvent.Closed == !(Keep || Fake));
	CHECK((gEfiVirtualNotifyEvent != NULL) == (Keep || Fake));
	if (Keep || Fake)
	{
		VirtualEvent.Notify(&VirtualEvent, NULL);
		CHECK(gEfiGoneVirtual && gEfiVirtualNotifyEvent == NULL);
		CHECK(Conversions == (Keep ? 2 : 1));
	}
	Probe(Keep);
}

int main(int argc, char** argv)
{
	if (argc != 4)
		return 2;
	const char* Case = argv[1];
	EFIMIRAGE_DSE_BYPASS_TYPE Mode = (EFIMIRAGE_DSE_BYPASS_TYPE)atoi(argv[2]);
	BOOLEAN Fake = atoi(argv[3]) != 0;
	FailVirtualEvent = strcmp(Case, "initialize-failure") == 0;
	if (strcmp(Case, "stale-state-init") == 0)
	{
		StateFlags = EFIMIRAGE_STATE_FLAG_FAKE_VBS;
		FailStateWrite = TRUE;
		if (setjmp(ResetExit) == 0)
			Initialize(Mode, Fake);
		CHECK(ResetCalled);
		return Failures == 0 ? 0 : 1;
	}
	Initialize(Mode, Fake);
	if (FailVirtualEvent)
		CheckUnloaded();
	else if (strcmp(Case, "stale-state-config") == 0)
	{
		StateFlags = EFIMIRAGE_STATE_FLAG_FAKE_VBS;
		FailStateWrite = TRUE;
		EFIMIRAGE_CONFIGURATION_DATA Config = gDriverConfig;
		Config.FakeVbs = 0;
		if (setjmp(ResetExit) == 0)
			DriverConfigure(&Config);
		CHECK(ResetCalled);
	}
	else if (strcmp(Case, "configuration-version") == 0)
	{
		EFIMIRAGE_CONFIGURATION_DATA Config = gDriverConfig;
		Config.Version = 0x100; // 2.1.0 treated FakeDma as reserved.
		Config.FakeDma = 1;
		CHECK(DriverConfigure(&Config) == EFI_INCOMPATIBLE_VERSION);
		CHECK(gDriverConfig.FakeDma == 0);
		Config.Version = EFIMIRAGE_CONFIGURATION_DATA_VERSION;
		CHECK(DriverConfigure(&Config) == EFI_SUCCESS);
		CHECK(gDriverConfig.FakeDma == 1);
		CHECK(EfiMirageUnload(gImageHandle) == EFI_SUCCESS);
		CheckUnloaded();
	}
	else if (strcmp(Case, "vbs-state") == 0)
	{
		EFIMIRAGE_CONFIGURATION_DATA Config = gDriverConfig;
		Config.FakeVbs = 1;
		CHECK(DriverConfigure(&Config) == EFI_SUCCESS);
		CONST UINT32 SecureBootFlag = Fake ? EFIMIRAGE_STATE_FLAG_FAKE_SECURE_BOOT : 0;
		CHECK(StateFlags == SecureBootFlag);
		CHECK(SetVbsPolicyConfirmed(TRUE) == EFI_SUCCESS);
		CHECK(StateFlags == (SecureBootFlag | EFIMIRAGE_STATE_FLAG_FAKE_VBS));
		CHECK(SetVbsPolicyConfirmed(FALSE) == EFI_SUCCESS);
		CHECK(StateFlags == SecureBootFlag);
		CHECK(SetVbsPolicyConfirmed(TRUE) == EFI_SUCCESS);
		CHECK(StateFlags == (SecureBootFlag | EFIMIRAGE_STATE_FLAG_FAKE_VBS));
		FailStateDelete = TRUE;
		CHECK(EfiMirageUnload(gImageHandle) == EFI_WRITE_PROTECTED);
		CHECK(StateFlags & EFIMIRAGE_STATE_FLAG_FAKE_VBS);
		CHECK(BootServices.StartImage == HookedStartImage);
		if (!Fake)
		{
			if (setjmp(ResetExit) == 0)
				ClearVbsPolicyOrReset();
			CHECK(ResetCalled);
			CHECK(StateFlags & EFIMIRAGE_STATE_FLAG_FAKE_VBS);
		}
		FailStateDelete = FALSE;
		CHECK(EfiMirageUnload(gImageHandle) == EFI_SUCCESS);
		CHECK(StateFlags == 0);
		CheckUnloaded();
	}
	else if (strcmp(Case, "normal-unload") == 0 || strcmp(Case, "unload-inflight") == 0)
	{
		if (strcmp(Case, "unload-inflight") == 0)
		{
			Other.Action = UnloadInFlight;
			Start(&Other);
			Probe(FALSE);
			Other.Action = Return;
			UINTN Size = 777;
			CHECK(BootServices.StartImage(&Other, &Size, NULL) == Other.StartStatus);
			CHECK(Size == sizeof(ExitMessage));
		}
		CHECK(EfiMirageUnload(gImageHandle) == EFI_SUCCESS);
		CheckUnloaded();
	}
	else
	{
		BOOLEAN Tracked = strcmp(Case, "windows-without-load") != 0;
		if (Tracked)
			LoadWindows();
		Windows.Eligible = Tracked && Mode == DSE_DISABLE_SETVARIABLE_HOOK;
		MOCK_IMAGE* Active = &Other;
		BOOLEAN Keep = FALSE;
		Other.Action = ExitBoot;
		if (strcmp(Case, "load-only") == 0)
			Active = NULL;
		else if (strcmp(Case, "return-failure") == 0 || strcmp(Case, "return-success") == 0)
		{
			Windows.StartStatus = strcmp(Case, "return-success") == 0 ? EFI_SUCCESS : EFI_LOAD_ERROR;
			Start(&Windows);
			Probe(FALSE);
		}
		else if (strcmp(Case, "active-windows") == 0 || !Tracked)
		{
			Windows.Action = ExitBoot;
			Active = &Windows;
			Keep = Windows.Eligible;
		}
		else if (strcmp(Case, "reused-handle") == 0)
		{
			Windows.Type = Unknown;
			Windows.Eligible = FALSE;
			Windows.Action = ExitBoot;
			Active = &Windows;
		}
		else if (strcmp(Case, "unknown") == 0 || strcmp(Case, "bootmgr") == 0)
			Other.Type = strcmp(Case, "bootmgr") == 0 ? Bootmgr : Unknown;
		else if (strcmp(Case, "protocol-failure") == 0 || strcmp(Case, "protocol-null") == 0)
		{
			Other.Type = BootmgfwEfi;
			Other.ProtocolStatus = strcmp(Case, "protocol-failure") == 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
			Other.NullProtocol = strcmp(Case, "protocol-null") == 0;
		}
		else if (strcmp(Case, "nested-return") == 0 || strcmp(Case, "nested-ebs") == 0 ||
			strcmp(Case, "nested-failed-protocol") == 0 || strcmp(Case, "nested-winload-ebs") == 0)
		{
			BOOLEAN ChildExits = strcmp(Case, "nested-ebs") == 0 || strcmp(Case, "nested-winload-ebs") == 0;
			BOOLEAN ChildIsWinload = strcmp(Case, "nested-winload-ebs") == 0;
			Windows.Action = ChildExits ? NestedExit : NestedReturn;
			Windows.Child = &Other;
			Other.Action = ChildExits ? ExitBoot : Return;
			if (ChildIsWinload)
			{
				// A WIM boot enters winload.efi under bootmgr.efi; that is still a Windows boot
				// attempt when boot services end, so the memory-access hook must survive it.
				Other.Type = WinloadEfi;
				Other.Eligible = Windows.Eligible;
			}
			if (strcmp(Case, "nested-failed-protocol") == 0)
			{
				Other.Type = BootmgfwEfi;
				Other.ProtocolStatus = EFI_UNSUPPORTED;
			}
			Active = &Windows;
			Keep = (ChildIsWinload || !ChildExits) && Windows.Eligible;
		}
		else
			return 2;
		UINTN Queries = HandleQueries, Types = Classifications;
		CheckBootExit(Active, Keep, Fake);
		if (Active != NULL)
		{
			CHECK(HandleQueries > Queries);
			if (Active->ProtocolStatus != EFI_SUCCESS || Active->NullProtocol)
				CHECK(Classifications == Types);
			else
				CHECK(Classifications > Types);
		}
	}
	return Failures == 0 ? 0 : 1;
}
