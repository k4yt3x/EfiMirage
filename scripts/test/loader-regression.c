// Native protocol mocks exercise the actual Loader.c routines, not a second parser.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NULL
#define _PCD_GET_MODE_32_PcdProgressCodeOsLoaderLoad 0
#define _PCD_GET_MODE_32_PcdProgressCodeOsLoaderStart 0
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "Loader.c"
#pragma GCC diagnostic pop

static unsigned Failures;
#define CHECK(Expression) do { if (!(Expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #Expression); ++Failures; \
} } while (0)

EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_HANDLE gImageHandle = (EFI_HANDLE)1;
static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES* gBS = &BootServices;
static EFI_LOADED_IMAGE_PROTOCOL LoadedImage;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL FileSystem;
static EFI_FILE_PROTOCOL Volume, File;
static UINT8 Input[8192];
static UINTN InputSize, RequestedSize, Reads, FileCloses, VolumeCloses, Allocations;
static BOOLEAN Missing, ReadError, ReadOnly;
static CHAR16 OpenedPath[1024];
static UINTN MarkerOpens, MarkerWrites, Diagnostics;
static CHAR16 Diagnostic[1024];
static UINT8 DevicePath[4096];
static UINTN DevicePathUsed;

VOID* EFIAPI AllocatePool(UINTN Size)
{
	VOID* Result = malloc(Size);
	if (Result != NULL)
		++Allocations;
	return Result;
}

VOID* EFIAPI AllocateZeroPool(UINTN Size)
{
	VOID* Result = AllocatePool(Size);
	if (Result != NULL)
		memset(Result, 0, Size);
	return Result;
}

VOID EFIAPI FreePool(VOID* Buffer)
{
	CHECK(Buffer != NULL && Allocations > 0);
	--Allocations;
	free(Buffer);
}

VOID* EFIAPI CopyMem(VOID* Destination, CONST VOID* Source, UINTN Length)
{
	return memmove(Destination, Source, Length);
}

UINTN EFIAPI StrLen(CONST CHAR16* String)
{
	UINTN Length = 0;
	while (String[Length] != 0)
		++Length;
	return Length;
}

CHAR16 EFIAPI CharToUpper(CHAR16 Character)
{
	return Character >= L'a' && Character <= L'z' ? Character - (L'a' - L'A') : Character;
}

CHAR16* EFIAPI StrStr(CONST CHAR16* String, CONST CHAR16* Search)
{
	UINTN Length = StrLen(Search);
	for (; *String != 0; ++String)
		if (StrLen(String) >= Length && memcmp(String, Search, Length * sizeof(CHAR16)) == 0)
			return (CHAR16*)String;
	return Length == 0 ? (CHAR16*)String : NULL;
}

UINTN EFIAPI Print(CONST CHAR16* Format, ...)
{
	++Diagnostics;
	CHECK(StrLen(Format) < ARRAY_SIZE(Diagnostic));
	CopyMem(Diagnostic, Format, (StrLen(Format) + 1) * sizeof(CHAR16));
	return StrLen(Format);
}

UINT8 EFIAPI DevicePathType(CONST VOID* Node)
{
	return ((CONST EFI_DEVICE_PATH_PROTOCOL*)Node)->Type;
}

UINT8 EFIAPI DevicePathSubType(CONST VOID* Node)
{
	return ((CONST EFI_DEVICE_PATH_PROTOCOL*)Node)->SubType;
}

UINTN EFIAPI DevicePathNodeLength(CONST VOID* Node)
{
	CONST EFI_DEVICE_PATH_PROTOCOL* Header = Node;
	return Header->Length[0] | ((UINTN)Header->Length[1] << 8);
}

EFI_DEVICE_PATH_PROTOCOL* EFIAPI NextDevicePathNode(CONST VOID* Node)
{
	return (EFI_DEVICE_PATH_PROTOCOL*)((UINT8*)Node + DevicePathNodeLength(Node));
}

BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID* Node)
{
	return DevicePathType(Node) == END_DEVICE_PATH_TYPE &&
		DevicePathSubType(Node) == END_ENTIRE_DEVICE_PATH_SUBTYPE;
}

// EDK2's presentation form separates file path nodes with '/', not a filesystem '\\'.
CHAR16* EFIAPI ConvertDevicePathToText(CONST EFI_DEVICE_PATH_PROTOCOL* Node,
	BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts)
{
	(void)DisplayOnly;
	(void)AllowShortcuts;
	CHAR16* Result = AllocateZeroPool(4096);
	UINTN Used = 0;
	while (!IsDevicePathEnd(Node))
	{
		if (Used != 0)
			Result[Used++] = L'/';
		CONST CHAR16* Text = DevicePathType(Node) == MEDIA_DEVICE_PATH &&
			DevicePathSubType(Node) == MEDIA_FILEPATH_DP
			? ((CONST FILEPATH_DEVICE_PATH*)Node)->PathName : L"VenHw(unsupported)";
		UINTN Length = StrLen(Text);
		CopyMem(Result + Used, Text, Length * sizeof(CHAR16));
		Used += Length;
		Node = NextDevicePathNode(Node);
	}
	return Result;
}

static EFI_STATUS EFIAPI MockOpenProtocol(EFI_HANDLE Handle, EFI_GUID* Protocol,
	VOID** Interface, EFI_HANDLE Agent, EFI_HANDLE Controller, UINT32 Attributes)
{
	(void)Agent;
	(void)Controller;
	(void)Attributes;
	CHECK(Handle == gImageHandle);
	CHECK(Protocol == &gEfiLoadedImageProtocolGuid);
	*Interface = &LoadedImage;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockHandleProtocol(EFI_HANDLE Handle, EFI_GUID* Protocol, VOID** Interface)
{
	CHECK(Handle == LoadedImage.DeviceHandle);
	CHECK(Protocol == &gEfiSimpleFileSystemProtocolGuid);
	*Interface = &FileSystem;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockLocateHandles(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID* Protocol,
	VOID* Key, UINTN* Count, EFI_HANDLE** Handles)
{
	(void)SearchType;
	(void)Protocol;
	(void)Key;
	*Count = 0;
	*Handles = NULL;
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This, EFI_FILE_PROTOCOL** Root)
{
	CHECK(This == &FileSystem);
	*Root = &Volume;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockOpen(EFI_FILE_PROTOCOL* This, EFI_FILE_PROTOCOL** NewHandle,
	CHAR16* Name, UINT64 Mode, UINT64 Attributes)
{
	(void)Attributes;
	CHECK(This == &Volume);
	if ((Mode & EFI_FILE_MODE_CREATE) != 0)
	{
		++MarkerOpens;
		CHECK(StrLen(Name) < ARRAY_SIZE(OpenedPath));
		CopyMem(OpenedPath, Name, (StrLen(Name) + 1) * sizeof(CHAR16));
		if (ReadOnly)
			return EFI_WRITE_PROTECTED;
	}
	else
	{
		CHECK(memcmp(Name, EFIMIRAGE_CONFIG_FILENAME, sizeof(EFIMIRAGE_CONFIG_FILENAME)) == 0);
		if (Missing)
			return EFI_NOT_FOUND;
	}
	*NewHandle = &File;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockClose(EFI_FILE_PROTOCOL* This)
{
	if (This == &File)
		++FileCloses;
	else
	{
		CHECK(This == &Volume);
		++VolumeCloses;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockRead(EFI_FILE_PROTOCOL* This, UINTN* Size, VOID* Buffer)
{
	CHECK(This == &File);
	++Reads;
	RequestedSize = *Size;
	if (*Size > InputSize)
		*Size = InputSize;
	CopyMem(Buffer, Input, *Size);
	return ReadError ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockWrite(EFI_FILE_PROTOCOL* This, UINTN* Size, VOID* Buffer)
{
	static const char Expected[] = "loader reached UefiMain\r\n";
	CHECK(This == &File);
	CHECK(*Size == sizeof(Expected) - 1);
	CHECK(memcmp(Buffer, Expected, sizeof(Expected) - 1) == 0);
	++MarkerWrites;
	return EFI_SUCCESS;
}

static void Initialize(void)
{
	BootServices.OpenProtocol = MockOpenProtocol;
	BootServices.HandleProtocol = MockHandleProtocol;
	BootServices.LocateHandleBuffer = MockLocateHandles;
	FileSystem.OpenVolume = MockOpenVolume;
	LoadedImage.DeviceHandle = (EFI_HANDLE)2;
	Volume.Open = MockOpen;
	Volume.Close = MockClose;
	File.Close = MockClose;
	File.Read = MockRead;
	File.Write = MockWrite;
}

static EFIMIRAGE_CONFIGURATION_DATA InitialConfig(void)
{
	EFIMIRAGE_CONFIGURATION_DATA Config;
	memset(&Config, 0xA5, sizeof(Config));
	Config.Version = EFIMIRAGE_CONFIGURATION_DATA_VERSION;
	Config.DseBypassMethod = DSE_DISABLE_SETVARIABLE_HOOK;
	Config.WaitForKeyPress = FALSE;
	Config.FakeSecureBoot = TRUE;
	return Config;
}

static void CheckConfig(const char* Case)
{
	static const char Text[] = "DseBypass = none\nWaitForKeyPress = yes\nFakeSecureBoot = 0\n";
	EFIMIRAGE_CONFIGURATION_DATA Config = InitialConfig();
	EFIMIRAGE_CONFIGURATION_DATA Expected = Config;
	BOOLEAN Accepted = TRUE;
	if (strcmp(Case, "ascii") == 0 || strcmp(Case, "utf8-bom") == 0 ||
		strcmp(Case, "exact-limit") == 0 || strcmp(Case, "oversize-prefix") == 0 ||
		strcmp(Case, "read-error") == 0 || strcmp(Case, "missing") == 0)
	{
		if (strcmp(Case, "utf8-bom") == 0)
		{
			Input[InputSize++] = 0xEF;
			Input[InputSize++] = 0xBB;
			Input[InputSize++] = 0xBF;
		}
		CopyMem(Input + InputSize, Text, sizeof(Text) - 1);
		InputSize += sizeof(Text) - 1;
		if (strcmp(Case, "exact-limit") == 0 || strcmp(Case, "oversize-prefix") == 0)
		{
			Input[InputSize++] = '#';
			memset(Input + InputSize, 'x', 4096 - InputSize);
			InputSize = 4096;
			if (strcmp(Case, "oversize-prefix") == 0)
				Input[InputSize++] = '\n';
		}
	}
	else if (strcmp(Case, "oversize-crossing-value") == 0)
	{
		static const char Ending[] = "\nFakeSecureBoot = 0";
		memset(Input, ' ', 4096);
		CopyMem(Input + 4096 - (sizeof(Ending) - 1), Ending, sizeof(Ending) - 1);
		Input[4096] = 'x';
		InputSize = 4097;
	}
	else if (strcmp(Case, "utf16") == 0 || strcmp(Case, "utf16-bom") == 0 ||
		strcmp(Case, "utf16-exact-limit") == 0)
	{
		if (strcmp(Case, "utf16") != 0)
		{
			Input[InputSize++] = 0xFF;
			Input[InputSize++] = 0xFE;
		}
		for (UINTN i = 0; i < sizeof(Text) - 1; ++i)
		{
			Input[InputSize++] = Text[i];
			Input[InputSize++] = 0;
		}
		if (strcmp(Case, "utf16-exact-limit") == 0)
		{
			Input[InputSize++] = '#';
			Input[InputSize++] = 0;
			while (InputSize < 4096)
			{
				Input[InputSize++] = 'x';
				Input[InputSize++] = 0;
			}
		}
	}
	else
	{
		CHECK(FALSE);
		return;
	}
	Missing = strcmp(Case, "missing") == 0;
	ReadError = strcmp(Case, "read-error") == 0;
	Accepted = !Missing && !ReadError && InputSize <= 4096;
	if (Accepted)
	{
		Expected.DseBypassMethod = DSE_DISABLE_NONE;
		Expected.WaitForKeyPress = TRUE;
		Expected.FakeSecureBoot = FALSE;
	}
	CHECK(LoadConfigurationFile(&Config) == Accepted);
	CHECK(memcmp(&Config, &Expected, sizeof(Config)) == 0);
	CHECK(Reads == (Missing ? 0 : 1));
	if (!Missing)
		CHECK(RequestedSize <= 4097);
	CHECK(FileCloses == (Missing ? 0 : 1));
	CHECK(VolumeCloses == 1);
	CHECK(Allocations == 0);
	if (InputSize > 4096 || ReadError)
	{
		CHECK(Diagnostics > 0);
		CHECK(StrStr(Diagnostic, L"Applying") == NULL);
	}
}

static void AddNode(const CHAR16* Name)
{
	EFI_DEVICE_PATH_PROTOCOL* Header = (EFI_DEVICE_PATH_PROTOCOL*)(DevicePath + DevicePathUsed);
	UINTN Size = SIZE_OF_FILEPATH_DEVICE_PATH + (StrLen(Name) + 1) * sizeof(CHAR16);
	Header->Type = MEDIA_DEVICE_PATH;
	Header->SubType = MEDIA_FILEPATH_DP;
	Header->Length[0] = (UINT8)Size;
	Header->Length[1] = (UINT8)(Size >> 8);
	CopyMem(DevicePath + DevicePathUsed + SIZE_OF_FILEPATH_DEVICE_PATH, Name,
		(StrLen(Name) + 1) * sizeof(CHAR16));
	DevicePathUsed += Size;
}

static void CheckMarker(const char* Case)
{
	const CHAR16* Expected = L"\\EFI\\Boot\\loader-ran.txt";
	if (strcmp(Case, "root") == 0)
	{
		AddNode(L"\\Loader.efi");
		Expected = L"\\loader-ran.txt";
	}
	else if (strcmp(Case, "root-relative") == 0)
	{
		AddNode(L"Loader.efi");
		Expected = L"\\loader-ran.txt";
	}
	else if (strcmp(Case, "single") == 0 || strcmp(Case, "read-only") == 0)
		AddNode(L"\\EFI\\Boot\\bootx64.efi");
	else if (strcmp(Case, "two") == 0)
	{
		AddNode(L"\\EFI\\Boot");
		AddNode(L"bootx64.efi");
	}
	else if (strcmp(Case, "three") == 0 || strcmp(Case, "relative-directories") == 0)
	{
		AddNode(strcmp(Case, "relative-directories") == 0 ? L"EFI" : L"\\EFI");
		AddNode(L"Boot");
		AddNode(L"bootx64.efi");
	}
	else if (strcmp(Case, "trailing-directory") == 0)
	{
		AddNode(L"\\EFI\\");
		AddNode(L"Boot\\");
		AddNode(L"bootx64.efi");
	}
	else if (strcmp(Case, "absolute-reset") == 0)
	{
		AddNode(L"\\Other");
		AddNode(L"Ignored");
		AddNode(L"\\EFI\\Boot");
		AddNode(L"bootx64.efi");
	}
	else if (strcmp(Case, "absolute-root-reset") == 0)
	{
		AddNode(L"\\Other");
		AddNode(L"\\Loader.efi");
		Expected = L"\\loader-ran.txt";
	}
	else if (strcmp(Case, "unsupported") == 0)
	{
		AddNode(L"\\EFI\\Boot");
		DevicePath[0] = HARDWARE_DEVICE_PATH;
		DevicePath[1] = HW_VENDOR_DP;
		AddNode(L"bootx64.efi");
		Expected = NULL;
	}
	else if (strcmp(Case, "unsupported-later") == 0)
	{
		AddNode(L"\\EFI\\Boot");
		UINTN Offset = DevicePathUsed;
		AddNode(L"bootx64.efi");
		DevicePath[Offset] = HARDWARE_DEVICE_PATH;
		DevicePath[Offset + 1] = HW_VENDOR_DP;
		Expected = NULL;
	}
	else if (strcmp(Case, "multiple-instances") == 0)
	{
		AddNode(L"\\EFI\\Boot\\bootx64.efi");
		EFI_DEVICE_PATH_PROTOCOL EndInstance = { END_DEVICE_PATH_TYPE, END_INSTANCE_DEVICE_PATH_SUBTYPE, { 4, 0 } };
		CopyMem(DevicePath + DevicePathUsed, &EndInstance, sizeof(EndInstance));
		DevicePathUsed += sizeof(EndInstance);
		AddNode(L"\\Loader.efi");
		Expected = NULL;
	}
	else if (strcmp(Case, "unterminated") == 0 || strcmp(Case, "short-node") == 0)
	{
		AddNode(L"\\Loader.efi");
		if (strcmp(Case, "unterminated") == 0)
			DevicePath[DevicePathUsed - 2] = 'x';
		else
			DevicePath[2] = SIZE_OF_FILEPATH_DEVICE_PATH;
		Expected = NULL;
	}
	else if (strcmp(Case, "missing") == 0 || strcmp(Case, "empty") == 0)
		Expected = NULL;
	else
	{
		CHECK(FALSE);
		return;
	}
	EFI_DEVICE_PATH_PROTOCOL End = { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { 4, 0 } };
	CopyMem(DevicePath + DevicePathUsed, &End, sizeof(End));
	LoadedImage.FilePath = strcmp(Case, "missing") == 0 ? NULL : (EFI_DEVICE_PATH_PROTOCOL*)DevicePath;
	ReadOnly = strcmp(Case, "read-only") == 0;
	TouchMarkerFile();
	CHECK(MarkerOpens == (Expected == NULL ? 0 : 1));
	CHECK(MarkerWrites == (Expected == NULL || ReadOnly ? 0 : 1));
	if (Expected != NULL)
	{
		CHECK(StrLen(OpenedPath) == StrLen(Expected));
		CHECK(memcmp(OpenedPath, Expected, (StrLen(Expected) + 1) * sizeof(CHAR16)) == 0);
	}
	CHECK(FileCloses == (Expected == NULL || ReadOnly ? 0 : 1));
	CHECK(VolumeCloses == (Expected == NULL ? 0 : 1));
	CHECK(Allocations == 0);
}

int main(int argc, char** argv)
{
	if (argc != 3)
		return 2;
	Initialize();
	if (strcmp(argv[1], "config") == 0)
		CheckConfig(argv[2]);
	else if (strcmp(argv[1], "marker") == 0)
		CheckMarker(argv[2]);
	else
		return 2;
	return Failures == 0 ? 0 : 1;
}
