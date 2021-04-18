#include <Windows.h>
#include <VersionHelpers.h>
#include <shellapi.h>
#include <commdlg.h>
#include <Shobjidl.h>
#include <PsApi.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <map>
#include <set>

#include "../Common/Windows/Windows_Symbol.hpp"

#pragma warning (disable:6262) // stack usage warning
#pragma warning (disable:26812) // unscoped enum warning
#pragma warning (disable:28159) // GetTickCount() warning

#define AUDCLNT_S_NO_SINGLE_PROCESS AUDCLNT_SUCCESS (0x00d)

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
    const wchar_t name [] = L"TRIMCORE.TaskbarSoundBadges";
    const VS_FIXEDFILEINFO * version = nullptr;

#if defined(_M_ARM64)
    const wchar_t dllname [] = L"TbSndBgAA.dll";
#elif defined(_WIN64)
    const wchar_t dllname [] = L"TbSndBg64.dll";
#else
    const wchar_t dllname [] = L"TbSndBg32.dll";
#endif

    HMENU menu = NULL;
    HWND window = NULL;
    UINT WM_Application = WM_NULL;
    UINT WM_TaskbarCreated = WM_NULL;
    HHOOK hook = NULL;
    HANDLE wait = NULL;
    HICON icons [3] = {};

    LRESULT CALLBACK wndproc (HWND, UINT, WPARAM, LPARAM);
    WNDCLASS wndclass = {
        0, wndproc, 0, 0,
        reinterpret_cast <HINSTANCE> (&__ImageBase),
        NULL, NULL, NULL, NULL, name
    };
    NOTIFYICONDATA nid = {
        sizeof (NOTIFYICONDATA), NULL, 1u, NIF_MESSAGE | NIF_STATE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
        WM_APP, NULL, { 0 }, 0u, 0u, { 0 }, { NOTIFYICON_VERSION_4 }, { 0 }, 0, { 0,0,0,{0} }, NULL
    };
    bool EnablePrivilege (LPCTSTR lpszPrivilege) {
        HANDLE hToken;
        if (OpenProcessToken (GetCurrentProcess (), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            LUID luid;
            if (LookupPrivilegeValue (NULL, lpszPrivilege, &luid)) {
                TOKEN_PRIVILEGES tp = { 1, { luid, SE_PRIVILEGE_ENABLED } };
                if (AdjustTokenPrivileges (hToken, FALSE, &tp, sizeof (TOKEN_PRIVILEGES), NULL, NULL)) {
                    return GetLastError () != ERROR_NOT_ALL_ASSIGNED;
                }
            }
        }
        return false;
    }

    enum Command : WPARAM {
        NoCommand = (WPARAM) -1,
        ShowCommand = 0x00,
        HideCommand = 0x01,
        SubTaskCommand = 0xE0,
        TerminateCommand = 0xFF,
    };

    struct VS_HEADER {
        WORD wLength;
        WORD wValueLength;
        WORD wType;
    };
    struct StringSet {
        const wchar_t * data;
        std::uint16_t   size;

        const wchar_t * operator [] (const wchar_t * name) const {
            if (this->data) {

                const VS_HEADER * header;
                auto p = this->data;
                auto e = this->data + this->size;

                while ((p < e) && ((header = reinterpret_cast <const VS_HEADER *> (p))->wLength != 0)) {

                    auto length = header->wLength / 2;
                    if (std::wcscmp (p + 3, name) == 0) {
                        return p + length - header->wValueLength;
                    }

                    p += length;
                    if (length % 2) {
                        ++p;
                    }
                }
            }
            return nullptr;
        }
    } strings;

    bool FirstInstance () {
        return CreateMutex (NULL, FALSE, name)
            && GetLastError () != ERROR_ALREADY_EXISTS;
    }

    Command ParseCommand (const wchar_t * argument) {
        while (*argument == L'/' || *argument == L'-') {
            ++argument;
        }
        if (std::wcscmp (argument, L"terminate") == 0) return TerminateCommand;
        if (std::wcscmp (argument, L"hide") == 0) return HideCommand;
        if (std::wcscmp (argument, L"show") == 0) return ShowCommand;
        if (std::wcscmp (argument, L"sub") == 0) return SubTaskCommand;

        return NoCommand;
    }

    LPWSTR * argw = NULL;

    Command ParseCommandLine (LPWSTR lpCmdLine, int * argument) {
        int argc;
        argw = CommandLineToArgvW (lpCmdLine, &argc);

        for (auto i = 0; i < argc; ++i) {
            auto cmd = ParseCommand (argw [i]);

            switch (cmd) {
                case NoCommand:
                    continue;

                case SubTaskCommand:
                    ++i;
                    if (i < argc) {
                        if (argument) {
                            *argument = i;
                        }
                    } else {
                        cmd = NoCommand;
                    }

                    [[ fallthrough ]];

                default:
                    return cmd;
            }
        }
        return NoCommand;
    }

    BOOL (WINAPI * ptrIsWow64Process2) (HANDLE, USHORT * ProcessMachine, USHORT * NativeMachine) = NULL;
    HRESULT (WINAPI * ptrIsWow64GuestMachineSupported) (USHORT WowGuestMachine, BOOL * MachineIsSupported) = NULL;

#ifdef _WIN64
    bool IsGuestSupported (USHORT machine) {
        if (ptrIsWow64GuestMachineSupported) {
            BOOL supported = false;
            if (ptrIsWow64GuestMachineSupported (machine, &supported) == S_OK)
                return supported;
            else
                return false;
        } else
            return true; // API missing, assume supported
    }
    bool IsWindowsBuildOrGreater (WORD wMajorVersion, WORD wMinorVersion, DWORD dwBuildNumber) {
        OSVERSIONINFOEXW osvi = { sizeof (osvi), 0, 0, 0, 0, { 0 }, 0, 0 };
        DWORDLONG mask = 0;

        mask = VerSetConditionMask (mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
        mask = VerSetConditionMask (mask, VER_MINORVERSION, VER_GREATER_EQUAL);
        mask = VerSetConditionMask (mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

        osvi.dwMajorVersion = wMajorVersion;
        osvi.dwMinorVersion = wMinorVersion;
        osvi.dwBuildNumber = dwBuildNumber;

        return VerifyVersionInfoW (&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, mask) != FALSE;
    }
#endif

    bool SpawnSubTasks () {
        Windows::Symbol (L"KERNEL32", ptrIsWow64GuestMachineSupported, "IsWow64GuestMachineSupported");

#ifdef _WIN64
        HANDLE current_process = GetCurrentProcess ();
        HANDLE real_current_process = NULL;
        if (DuplicateHandle (current_process, current_process, current_process, &real_current_process, SYNCHRONIZE, TRUE, 0)) {

            wchar_t exe [2 * MAX_PATH];
            wchar_t cmd [64];

            DWORD n = sizeof exe / sizeof exe [0];
            if (QueryFullProcessImageName (current_process, 0, exe, &n)) {

                // start x86-64 on AArch64

                const bool aa = ((exe [n - 6] == L'A') || (exe [n - 6] == L'a')) && ((exe [n - 5] == L'A') || (exe [n - 5] == L'a'));

#if defined(_M_ARM64)
                if (IsWindowsBuildOrGreater (10, 0, 21277)) {
                    if (aa) {
                        exe [n - 6] = L'6';
                        exe [n - 5] = L'4';

                        _snwprintf (cmd, sizeof cmd / sizeof cmd [0], L"TbSndBg64.exe sub %llx", (std::intptr_t) real_current_process);

                        STARTUPINFO si {};
                        si.cb = sizeof si;
                        PROCESS_INFORMATION pi {};
                        if (CreateProcess (exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
                            CloseHandle (pi.hProcess);
                            CloseHandle (pi.hThread);
                        }
                    }
                }
#endif
                // start x86-32 on x86-64 and AArch64

                if (IsGuestSupported (IMAGE_FILE_MACHINE_I386)) {
                    if (((exe [n - 6] == L'6') && (exe [n - 5] == L'4')) || aa) {
                        exe [n - 6] = L'3';
                        exe [n - 5] = L'2';

                        _snwprintf (cmd, sizeof cmd / sizeof cmd [0], L"TbSndBg32.exe sub %llx", (std::intptr_t) real_current_process);

                        STARTUPINFO si {};
                        si.cb = sizeof si;
                        PROCESS_INFORMATION pi {};
                        if (CreateProcess (exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
                            CloseHandle (pi.hProcess);
                            CloseHandle (pi.hThread);
                        }
                    }
                }
            }
        }
#endif
        return false;
    }

    bool SwitchToProperProcess () {
        HANDLE current_process = GetCurrentProcess ();
        if (Windows::Symbol (L"KERNEL32", ptrIsWow64Process2, "IsWow64Process2")) {
            USHORT process, machine;
            if (ptrIsWow64Process2 (current_process, &process, &machine)) {
                switch (machine) {
#if !defined(_M_ARM64)
                    case IMAGE_FILE_MACHINE_ARM64:
                        // TODO: start AA, on success return true
                        break;
#endif
#if !defined(_M_X64) 
                    case IMAGE_FILE_MACHINE_AMD64:
                        // TODO: start 64, on success return true
                        break;
#endif
                }
            }
        } else {
#if !defined(_M_X64) 
            BOOL wow = FALSE;
            if (IsWow64Process (current_process, &wow) && wow) {
                // TODO: start 64, on success return true
            }
#endif
        }
        return false;
    }

    void CALLBACK MasterTaskExit (PVOID, BOOLEAN) {
        PostMessage (window, WM_CLOSE, ERROR_PROCESS_IN_JOB, 0);
    }

    DWORD WIN32_FROM_HRESULT (HRESULT hr) {
        if ((hr & 0xFFFF0000) == MAKE_HRESULT (SEVERITY_ERROR, FACILITY_WIN32, 0)) {
            return HRESULT_CODE (hr);
        } else
            return (DWORD) hr;
    }
}

int APIENTRY wWinMain (_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    auto command = NoCommand;
    {
        int argument;
        command = ParseCommandLine (lpCmdLine, &argument);
        if (FirstInstance ()) {
            switch (command) {
                case SubTaskCommand:
                    return ERROR_PROCESS_NOT_IN_JOB;
                case TerminateCommand:
                case ShowCommand:
                    return ERROR_SERVICE_NOT_ACTIVE;
                case HideCommand: // start hidden
                    nid.dwState = NIS_HIDDEN;
                    nid.dwStateMask = NIS_HIDDEN;
                    break;
            }

#if !defined(_M_ARM64)
            if (SwitchToProperProcess ()) {
                return 0;
            }
#endif
        } else {
            switch (command) {
                case SubTaskCommand:
                    nid.uID = 0;
                    nid.dwState = NIS_HIDDEN;
                    nid.dwStateMask = NIS_HIDDEN;

                    // exit when parent process exist
                    RegisterWaitForSingleObject (&wait, (HANDLE) std::wcstoull (argw [argument], nullptr, 16), MasterTaskExit, NULL, INFINITE,
                                                 WT_EXECUTEINWAITTHREAD | WT_EXECUTELONGFUNCTION | WT_EXECUTEONLYONCE);
                    break;

                default:
                    SetLastError (0);
                    DWORD recipients = BSM_APPLICATIONS;
                    switch (command) {
                        case TerminateCommand:
                            if (EnablePrivilege (SE_TCB_NAME)) {
                                recipients |= BSM_ALLDESKTOPS;
                            }
                            break;
                        case NoCommand:
                            command = ShowCommand;
                    }
                    BroadcastSystemMessage (BSF_FORCEIFHUNG | BSF_IGNORECURRENTTASK, &recipients, RegisterWindowMessage (name), command, 0);
                    return GetLastError ();
            }
        }
        LocalFree (argw);
    }

    if (HRSRC hRsrc = FindResource (hInstance, MAKEINTRESOURCE (1), RT_VERSION)) {
        if (HGLOBAL hGlobal = LoadResource (hInstance, hRsrc)) {
            auto data = LockResource (hGlobal);
            auto size = SizeofResource (hInstance, hRsrc);

            struct VS_VERSIONINFO : public VS_HEADER {
                WCHAR szKey [sizeof "VS_VERSION_INFO"]; // 15 characters
                WORD  Padding1 [1];
                VS_FIXEDFILEINFO Value;
            };

            if (size >= sizeof (VS_VERSIONINFO)) {
                const auto * vi = static_cast <const VS_HEADER *> (data);
                const auto * vp = static_cast <const unsigned char *> (data)
                    + sizeof (VS_VERSIONINFO) + sizeof (VS_HEADER) - sizeof (VS_FIXEDFILEINFO)
                    + vi->wValueLength;

                if (!std::wcscmp (reinterpret_cast <const wchar_t *> (vp), L"StringFileInfo")) {
                    vp += sizeof (L"StringFileInfo");

                    strings.size = reinterpret_cast <const VS_HEADER *> (vp)->wLength / 2 - std::size_t (12);
                    strings.data = reinterpret_cast <const wchar_t *> (vp) + 12;
                }

                if (vi->wValueLength) {
                    auto p = reinterpret_cast <const DWORD *> (LockResource (hGlobal));
                    auto e = p + (size - sizeof (VS_FIXEDFILEINFO)) / sizeof (DWORD);

                    p = std::find (p, e, 0xFEEF04BDu);
                    if (p != e)
                        version = reinterpret_cast <const VS_FIXEDFILEINFO *> (p);
                }
            }
        }
    }

    if (!version || !strings.data) {
        return GetLastError ();
    }
    if (auto hr = CoInitializeEx (NULL, COINIT_MULTITHREADED); !SUCCEEDED (hr)) {
        return WIN32_FROM_HRESULT (hr);
    }

    {
        auto cx = GetSystemMetrics (SM_CXSMICON);
        auto cy = GetSystemMetrics (SM_CYSMICON);

        if (auto mmres = LoadLibrary (L"MMRES")) {
            LoadIconWithScaleDown (mmres, MAKEINTRESOURCE (3004), cx, cy, &icons [0]);
        }

        LoadIconWithScaleDown ((HINSTANCE) &__ImageBase, MAKEINTRESOURCE (2), cx, cy, &icons [1]);
        LoadIconWithScaleDown ((HINSTANCE) &__ImageBase, MAKEINTRESOURCE (3), cx, cy, &icons [2]);
    }

    if (auto atom = RegisterClass (&wndclass)) {
        menu = GetSubMenu (LoadMenu (hInstance, MAKEINTRESOURCE (1)), 0);
        if (menu) {
            SetMenuDefaultItem (menu, 0xF1, FALSE);
            CheckMenuRadioItem (menu, 0x11, 0x11, 0x11, MF_BYCOMMAND);

            WM_Application = RegisterWindowMessage (name);
            WM_TaskbarCreated = RegisterWindowMessage (L"TaskbarCreated");

            window = CreateWindow ((LPCTSTR) (std::intptr_t) atom, L"", 0, 0, 0, 0, 0, HWND_DESKTOP, NULL, hInstance, NULL);
            if (window) {
                ChangeWindowMessageFilterEx (window, WM_Application, MSGFLT_ALLOW, NULL);
                ChangeWindowMessageFilterEx (window, WM_TaskbarCreated, MSGFLT_ALLOW, NULL);

                if (command != SubTaskCommand) {
                    SpawnSubTasks ();
                }

                MSG message;
                while (GetMessage (&message, NULL, 0, 0) > 0) {
                    DispatchMessage (&message);
                }
                if (message.message == WM_QUIT) {
                    return (int) message.wParam;
                }
            }
        }
    }
    return GetLastError ();
}

namespace {
    bool StartHook () {
        auto flags = 0;
        if (IsWindows8OrGreater ()) {
            flags |= LOAD_LIBRARY_SEARCH_APPLICATION_DIR;
        }
        HMODULE dll = NULL;
        HOOKPROC proc = NULL;

        SetLastError (0);
        if (dll = LoadLibraryEx (dllname, NULL, flags)) {
            if (proc = (HOOKPROC) GetProcAddress (dll, "Hook")) {
                hook = SetWindowsHookEx (WH_CALLWNDPROC, proc, dll, 0);
                if (hook)
                    return true;
            }
        }
        return false;
    }
}

namespace {
    void about () {
        wchar_t text [4096];
        auto n = _snwprintf (text, sizeof text / sizeof text [0], L"%s - %s\n%s\n\n",
                             strings [L"ProductName"], strings [L"ProductVersion"], strings [L"LegalCopyright"]);
        int i = 0x10;
        int m = 0;
        while (m = LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), i, &text [n], sizeof text / sizeof text [0] - n)) {
            n += m;
            i++;
        }

        MSGBOXPARAMS box = {};
        box.cbSize = sizeof box;
        box.hwndOwner = HWND_DESKTOP;
        box.hInstance = reinterpret_cast <HINSTANCE> (&__ImageBase);
        box.lpszText = text;
        box.lpszCaption = strings [L"ProductName"];
        box.dwStyle = MB_USERICON;
        box.lpszIcon = MAKEINTRESOURCE (1);
        box.dwLanguageId = LANG_USER_DEFAULT;
        MessageBoxIndirect (&box);
    }

    void track (HWND hWnd, POINT pt) {
        UINT style = TPM_RIGHTBUTTON;
        BOOL align = FALSE;

        if (SystemParametersInfo (SPI_GETMENUDROPALIGNMENT, 0, &align, 0)) {
            if (align) {
                style |= TPM_RIGHTALIGN;
            }
        }

        SetForegroundWindow (hWnd);
        TrackPopupMenu (menu, style, pt.x, pt.y, 0, hWnd, NULL);
        Shell_NotifyIcon (NIM_SETFOCUS, &nid);
        PostMessage (hWnd, WM_NULL, 0, 0);
    }

    DWORD tray = ERROR_IO_PENDING;
    IMMDeviceEnumerator * deviceEnumerator = NULL;

    struct Entry {
        bool    playing; // state whether PID is playing
        USHORT  windows; // number of windows of the PID
        DWORD   t;
    };
    std::map <DWORD, Entry> data;

    void update () {
        if (nid.uID) {
            if (tray != ERROR_SUCCESS) {
                _snwprintf (nid.szTip, sizeof nid.szTip / sizeof nid.szTip [0], L"%s - %s\nERROR %u",
                            strings [L"ProductName"], strings [L"ProductVersion"], tray);

                // TODO: FormatMessage

            } else {
                const auto now = GetTickCount ();
                std::set <std::pair <USHORT, DWORD>> ordered;

                for (const auto & [pid, entry] : data) {
                    if (entry.playing || (now - entry.t < 10'000)) {
                        ordered.insert (std::make_pair (entry.windows, pid));
                    }
                }

                if (!ordered.empty ()) {
                    constexpr auto MAXTIP = sizeof nid.szTip / sizeof nid.szTip [0];

                    // tip title

                    std::size_t i;
                    if (ordered.size () > 1) {
                        wchar_t buffer [MAXTIP];
                        LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x02, buffer, MAXTIP);
                        i = _snwprintf (nid.szTip, MAXTIP, buffer, ordered.size ());
                    } else {
                        i = LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x01, nid.szTip, MAXTIP);
                    }

                    // list of processes

                    auto n = 0u;
                    for (auto & [windows, pid] : ordered) {
                        
                        // executable name

                        wchar_t exe [2 * MAX_PATH] = {};

                        if (pid) {
                            if (auto handle = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                                DWORD n = sizeof exe / sizeof exe [0];
                                if (QueryFullProcessImageName (handle, 0, exe, &n)) {
                                    if (auto basename = std::wcsrchr (exe, L'\\')) {
                                        std::wmemmove (exe, basename + 1, n - (basename - exe));
                                    }
                                }
                                CloseHandle (handle);
                            }

                            if (!exe [0]) {
                                _snwprintf (exe, 12, L"#%u", pid);
                            }
                        } else {
                            // TODO: load "System Sounds" text from session->GetDisplayName
                            _snwprintf (exe, 12, L"System");
                        }

                        // format row
                        //  - TODO: process name from versioninfo?

                        wchar_t buffer [MAXTIP];
                        auto length = _snwprintf (buffer, MAXTIP, L"\n\x2002%c\x2002%s", (windows > 1) ? L'\x266B' : L'\x266A', exe);

                        if (i + length + 2 < MAXTIP) {
                            i += length;
                            std::wcscat (nid.szTip, buffer);
                            ++n;

                        } else
                            break;
                    }
                    
                    // if not all, append ellipsis

                    if (n < ordered.size ()) {
                        nid.szTip [i + 0] = L'\n';
                        nid.szTip [i + 1] = L'\x2026';
                        nid.szTip [i + 2] = L'\0';
                    }
                } else {
                    _snwprintf (nid.szTip, sizeof nid.szTip / sizeof nid.szTip [0], L"%s - %s\n%s",
                                strings [L"ProductName"], strings [L"ProductVersion"], strings [L"CompanyName"]);
                }
            }

            Shell_NotifyIcon (NIM_MODIFY, &nid);
        }
    }

    UINT GetMenuRadioItem (HMENU menu, UINT first, UINT last, UINT flags) {
        for (UINT item = first; item < last + 1; ++item) {
            if (GetMenuState (menu, item, flags) & MF_CHECKED)
                return item;
        }
        return 0;
    }

    BOOL CALLBACK notify (_In_ HWND hwnd, _In_ LPARAM lParam) {
        auto & state = data [(DWORD) lParam];
        
        if (GetParent (hwnd) == HWND_DESKTOP) {
            auto icon = GetClassLongPtr (hwnd, GCLP_HICON);
            auto style = GetWindowLongPtr (hwnd, GWL_STYLE);
            auto extra = GetWindowLongPtr (hwnd, GWL_EXSTYLE);

            bool taskbar = (icon != NULL);

            if (extra & WS_EX_APPWINDOW) taskbar = true;
            if (style & WS_CHILD) taskbar = false;
            if (extra & WS_EX_TOOLWINDOW) taskbar = false;
            if ((style & WS_POPUP) && !(extra & WS_EX_APPWINDOW)) taskbar = false;

            if (taskbar) {
                
                DWORD pid;
                if (GetWindowThreadProcessId (hwnd, &pid)) {

                    if (pid == lParam) {
                        ++state.windows;

                        HICON icon = NULL;
                        if (state.playing) {
                            icon = icons [GetMenuRadioItem (menu, 0x11, 0x13, MF_BYCOMMAND) - 0x11];
                        }
                        SendMessage (hwnd, WM_Application, state.playing, (LPARAM) icon);
                    }
                }
            }
        }
        return TRUE;
    }

    LRESULT CALLBACK wndproc (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                if (nid.uID) {
                    nid.hWnd = hWnd;
                    LoadIconWithScaleDown ((HINSTANCE) &__ImageBase, MAKEINTRESOURCE (1), GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON), &nid.hIcon);

                    Shell_NotifyIcon (NIM_ADD, &nid);
                    Shell_NotifyIcon (NIM_SETVERSION, &nid);

                    update ();
                    SetTimer (hWnd, 1, 200, NULL);
                }

                if (StartHook ()) {
                    tray = ERROR_SUCCESS;
                } else {
                    tray = GetLastError ();
                }

                if (auto hr = CoCreateInstance (__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&deviceEnumerator)); !SUCCEEDED (hr)) {
                    tray = WIN32_FROM_HRESULT (hr);
                }

                update ();
                return 0;

            case WM_APP:
                switch (LOWORD (lParam)) {
                    case WM_LBUTTONDBLCLK:
                        wndproc (hWnd, WM_COMMAND, GetMenuDefaultItem (menu, FALSE, 0) & 0xFFFF, 0);
                        break;

                    case WM_CONTEXTMENU:
                        track (hWnd, { (short) LOWORD (wParam), (short) HIWORD (wParam) });
                }
                break;

            case WM_COMMAND:
                switch (wParam) {
                    case 0x11:
                    case 0x12:
                    case 0x13:
                        CheckMenuRadioItem (menu, 0x11, 0x13, (UINT) wParam, MF_BYCOMMAND);
                        for (auto & entry : data) {
                            entry.second.playing = false; // will cause timer to send new icon
                        }
                        break;

                    case 0xF1:
                        about ();
                        break;
                    case 0xFA:
                        ShellExecute (hWnd, NULL, L"https://www.trimcore.cz", NULL, NULL, SW_SHOWDEFAULT);
                        break;
                    case 0xFE:
                        if (nid.uID) {
                            nid.dwState = NIS_HIDDEN;
                            nid.dwStateMask = NIS_HIDDEN;
                            Shell_NotifyIcon (NIM_MODIFY, &nid);
                        }
                        break;
                    case 0xFF:
                        PostMessage (hWnd, WM_CLOSE, ERROR_SUCCESS, 0);
                }
                break;

            case WM_QUERYENDSESSION:
                SendMessage (hWnd, WM_CLOSE, ERROR_SHUTDOWN_IN_PROGRESS, 0);
                return TRUE;

            case WM_CLOSE:
                Shell_NotifyIcon (NIM_DELETE, &nid);
                KillTimer (hWnd, 1);
                DestroyWindow (hWnd);

                for (auto & [pid, entry] : data) {
                    if (entry.playing) {
                        entry.playing = 0;
                        EnumWindows (notify, LPARAM (pid));
                    }
                }

                if (hook != NULL) {
                    if (!UnhookWindowsHookEx (hook)) {
                        wParam = GetLastError ();
                    }
                }
                deviceEnumerator->Release ();
                deviceEnumerator = NULL;

                PostQuitMessage ((int) wParam);
                break;

            case WM_TIMER:
                if (wParam == 1) {
                    auto t = GetTickCount ();
                    bool change = false;
                    bool system_playing = false;

                    // TODO: use registration callbacks instead of enumeration

                    IMMDeviceCollection * deviceCollection;
                    if (SUCCEEDED (deviceEnumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &deviceCollection))) {

                        UINT numberOfDevices;
                        if (SUCCEEDED (deviceCollection->GetCount (&numberOfDevices))) {
                            for (UINT i = 0; i != numberOfDevices; ++i) {

                                IMMDevice * device;
                                if (SUCCEEDED (deviceCollection->Item (i, &device))) {

                                    IAudioSessionManager2 * audioSessionManager = NULL;
                                    if (SUCCEEDED (device->Activate (__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, reinterpret_cast <void **> (&audioSessionManager)))) {

                                        IAudioSessionEnumerator * sessionEnumerator = NULL;
                                        if (SUCCEEDED (audioSessionManager->GetSessionEnumerator (&sessionEnumerator))) {

                                            int numberOfSessions;
                                            if (SUCCEEDED (sessionEnumerator->GetCount (&numberOfSessions))) {
                                                for (int i = 0; i != numberOfSessions; ++i) {

                                                    IAudioSessionControl * ctrl = NULL;
                                                    if (SUCCEEDED (sessionEnumerator->GetSession (i, &ctrl))) {

                                                        IAudioSessionControl2 * session = NULL;
                                                        if (SUCCEEDED (ctrl->QueryInterface (&session))) {

                                                            AudioSessionState audioState;
                                                            if (SUCCEEDED (session->GetState (&audioState))) {

                                                                if (session->IsSystemSoundsSession () == S_OK) {
                                                                    if (audioState == AudioSessionStateActive) {
                                                                        system_playing = true;
                                                                    }
                                                                } else {
                                                                    DWORD pid = 0;
                                                                    if (SUCCEEDED (session->GetProcessId (&pid)) && (pid != 0)) {

                                                                        auto active = (audioState == AudioSessionStateActive);
                                                                        auto & state = data [pid];

                                                                        if (active) {
                                                                            state.t = t;
                                                                            change = true;
                                                                        }
                                                                        if (state.playing != active) {
                                                                            state.playing = active;
                                                                            state.windows = 0;
                                                                            change = true;

                                                                            EnumWindows (notify, LPARAM (pid));
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                            session->Release ();
                                                        }
                                                        ctrl->Release ();
                                                    }
                                                }
                                            }
                                            sessionEnumerator->Release ();
                                        }
                                        audioSessionManager->Release ();
                                    }
                                    device->Release ();
                                }
                            }
                        }
                        deviceCollection->Release ();
                    }
                    if (data [0].playing != system_playing) {
                        data [0].playing = system_playing;
                        data [0].t = t;
                        change = true;
                    }
                    if (change) {
                        update ();
                    }
                }
                break;

            default:
                if (message == WM_Application) {
                    switch (wParam) {
                        case ShowCommand:
                        case HideCommand:
                            if (nid.uID) {
                                nid.dwState = (wParam == HideCommand) ? NIS_HIDDEN : 0;
                                nid.dwStateMask = NIS_HIDDEN;
                                Shell_NotifyIcon (NIM_MODIFY, &nid);
                            }
                            break;
                        case TerminateCommand:
                            SendMessage (hWnd, WM_CLOSE, ERROR_SUCCESS, 0);
                            break;
                    }
                    return 0;
                } else
                    if (message == WM_TaskbarCreated) {
                        if (nid.uID) {
                            Shell_NotifyIcon (NIM_ADD, &nid);
                            Shell_NotifyIcon (NIM_SETVERSION, &nid);
                            update ();
                        }
                        return 0;
                    } else
                        return DefWindowProc (hWnd, message, wParam, lParam);
        }
        return 0;
    }
}
