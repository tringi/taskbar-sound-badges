#include <Windows.h>
#include <Shobjidl.h>
#include <cstdio>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
    UINT message = WM_NULL;
    HRESULT hresult = S_OK;
    bool initialized = false;
    wchar_t hint [63];

    HRESULT EnsureResources () {
        if (!initialized) {
            LoadString ((HINSTANCE) &__ImageBase, 1, hint, sizeof hint / sizeof hint [0]);
            hresult = CoInitializeEx (NULL, COINIT_MULTITHREADED);
            
            switch (hresult) {
                // case S_OK:
                case S_FALSE:
                case RPC_E_CHANGED_MODE:
                    hresult = S_OK;
                    break;
            }
            initialized = true;
        }
        return hresult;
    }
}

extern "C" __declspec (dllexport)
LRESULT CALLBACK Hook (int code, WPARAM wParamHook, LPARAM lParamHook) {
    if (code == HC_ACTION) {
        const auto msg = (const CWPSTRUCT *) lParamHook;

        if (message == WM_NULL) {
            message = RegisterWindowMessage (L"TRIMCORE.TaskbarSoundBadges");
            if (message) {
                ChangeWindowMessageFilter (message, MSGFLT_ADD);
            }
        }

        if (msg->message && (msg->message == message)) {

            auto result = EnsureResources ();
            if (result == S_OK) {

                ITaskbarList3 * taskbar = NULL;
                result = CoCreateInstance (CLSID_TaskbarList, NULL, CLSCTX_ALL, IID_ITaskbarList3, (void **) &taskbar);

                if (SUCCEEDED (result)) {
                    result = taskbar->HrInit ();

                    if (SUCCEEDED (result)) {
                        result = taskbar->SetOverlayIcon (msg->hwnd, (HICON) msg->lParam, msg->lParam ? hint : NULL);
                    }
                    taskbar->Release ();
                }
            }
            ReplyMessage (result);
        }
    }
    return CallNextHookEx (NULL, code, wParamHook, lParamHook);
}
