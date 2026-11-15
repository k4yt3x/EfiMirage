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
static BOOLEAN Missing, ReadError;
static UINTN Diagnostics;
static CHAR16 Diagnostic[1024];

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
	CHECK(Mode == EFI_FILE_MODE_READ);
	CHECK(memcmp(Name, EFIMIRAGE_CONFIG_FILENAME, sizeof(EFIMIRAGE_CONFIG_FILENAME)) == 0);
	if (Missing)
		return EFI_NOT_FOUND;
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

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	Initialize();
	CheckConfig(argv[1]);
	return Failures == 0 ? 0 : 1;
}
