; Forwarding stubs for version.dll exports (x64 MASM).
; g_origVersion[] is filled in DllMain from %SystemRoot%\System32\version.dll.

EXTERN g_origVersion:QWORD

STUB MACRO name, idx
name PROC
    jmp QWORD PTR [g_origVersion + idx * 8]
name ENDP
ENDM

.code
STUB Proxy_GetFileVersionInfoA,        0
STUB Proxy_GetFileVersionInfoByHandle, 1
STUB Proxy_GetFileVersionInfoExA,      2
STUB Proxy_GetFileVersionInfoExW,      3
STUB Proxy_GetFileVersionInfoSizeA,    4
STUB Proxy_GetFileVersionInfoSizeExA,  5
STUB Proxy_GetFileVersionInfoSizeExW,  6
STUB Proxy_GetFileVersionInfoSizeW,    7
STUB Proxy_GetFileVersionInfoW,        8
STUB Proxy_VerFindFileA,               9
STUB Proxy_VerFindFileW,               10
STUB Proxy_VerInstallFileA,            11
STUB Proxy_VerInstallFileW,            12
STUB Proxy_VerLanguageNameA,           13
STUB Proxy_VerLanguageNameW,           14
STUB Proxy_VerQueryValueA,             15
STUB Proxy_VerQueryValueW,             16
END
