// Case-sensitivity shim.
//
// EfiDSEFix's ntdll.h does `#include <Windows.h>`, which only resolves on a
// case-insensitive filesystem (i.e. Windows). On Linux/MinGW cross-compiles
// this shim is found first via -I and forwards to the real <windows.h>.
//
// The compatibility directory precedes the MinGW system headers on the include path.
#pragma once

#include <windows.h>
