#include <windows.h>
#include <Shobjidl.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <string>

// DLL 32b + 64b
//  - only loaded into GUI apps (hook)
//  - RegisterWindowMessage (...) -> message -> ITaskbarList3::SetOverlayIcon 

// ITaskbarList3::SetOverlayIcon 
/*{
bool result = false;
ITaskbarList * pTaskbarList = NULL;
if (SUCCEEDED (CoCreateInstance (CLSID_TaskbarList, NULL,
                                 CLSCTX_ALL, IID_ITaskbarList,
                                 (void **) &pTaskbarList))) {
    if (SUCCEEDED (pTaskbarList->HrInit ())) {
        result = pTaskbarList->SetOverlayIcon (...) == S_OK;
    };
    pTaskbarList->Release ();
};
return result;
};*/


/*class SessionCallbacks : IAudioSessionEvents {
public:
    virtual HRESULT STDMETHODCALLTYPE OnDisplayNameChanged (LPCWSTR NewDisplayName, LPCGUID EventContext) {
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE OnIconPathChanged (LPCWSTR NewIconPath, LPCGUID EventContext) {
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged (float NewVolume, BOOL NewMute, LPCGUID EventContext) {
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged (_In_  DWORD ChannelCount, _In_reads_ (ChannelCount)  float NewChannelVolumeArray [], _In_  DWORD ChangedChannel, LPCGUID EventContext) {
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE OnGroupingParamChanged (LPCGUID NewGroupingParam, LPCGUID EventContext) {
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE OnStateChanged (AudioSessionState NewState) {
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE OnSessionDisconnected (AudioSessionDisconnectReason DisconnectReason) {

        return S_OK;
    }
} cb; // */

extern "C" IMAGE_DOS_HEADER __ImageBase;

LRESULT CALLBACK wndproc (HWND hWnd, UINT message,
                          WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            SetTimer (hWnd, 1, 1000, NULL);
            return 0;
        
        case WM_TIMER: {
            auto mmres = LoadLibrary (L"MMRES");
            std::printf ("mmres: %p\n", mmres);
            auto icon = (HICON) LoadImage (mmres, MAKEINTRESOURCE (3004), IMAGE_ICON, 32, 32/*
                                           GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON)*/, LR_DEFAULTCOLOR);
            std::printf ("icon: %p\n", icon);

            ITaskbarList3 * taskbar = NULL;
            if (CoCreateInstance (CLSID_TaskbarList, NULL, CLSCTX_ALL, IID_ITaskbarList3, (void **) &taskbar) == S_OK) {
                std::printf ("taskbar: %p\n", taskbar);
                if (taskbar->HrInit () == S_OK) {
                    std::printf ("taskbar: %p HrInit, HWND: %p\n", taskbar, hWnd);
                    if (taskbar->SetOverlayIcon (hWnd, icon, L"Window is playing sound") == S_OK) {
                        std::printf ("taskbar overlay set\n");
                    }
                }
                taskbar->Release ();
            }
        } break;

        case WM_CLOSE:
            PostQuitMessage ((int) wParam);
            break;

        default:
            return DefWindowProc (hWnd, message, wParam, lParam);
    }
    return 0;
}

WNDCLASS wndclass = {
        0, wndproc, 0, 0,
        reinterpret_cast <HINSTANCE> (&__ImageBase),
        NULL, NULL, NULL, NULL, L"1"
};

int main () {
    CoInitializeEx (NULL, COINIT_MULTITHREADED);

    /*if (auto atom = RegisterClass (&wndclass)) {
        auto window = CreateWindow ((LPCTSTR) (std::intptr_t) atom, L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                    HWND_DESKTOP, NULL, reinterpret_cast <HINSTANCE> (&__ImageBase), NULL);
        if (window) {
            MSG message;
            while (GetMessage (&message, NULL, 0, 0) > 0) {
                DispatchMessage (&message);
            }
            if (message.message == WM_QUIT) {
                return (int) message.wParam;
            }
        }
    }// */



    IMMDevice * defaultDevice = NULL;
    IMMDeviceCollection * deviceCollection = NULL;

    while (true) {
        {
            IMMDeviceEnumerator * deviceEnumerator = NULL;
            CoCreateInstance (__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&deviceEnumerator));
            deviceEnumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &deviceCollection);

            deviceEnumerator->GetDefaultAudioEndpoint (eRender, eMultimedia, &defaultDevice);
            
            // ->RegisterEndpointNotificationCallback (...)
            // IMMNotificationClient ::OnDeviceAdded / Removed / StateChanged -> SetEvent (...)

            deviceEnumerator->Release ();
        }

        {
            IAudioEndpointVolume * endpointVolume = NULL;
            defaultDevice->Activate (__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, reinterpret_cast<void **>(&endpointVolume));
            defaultDevice->Release ();

            float volume;
            UINT channels;
            BOOL mute;
            endpointVolume->GetMasterVolumeLevelScalar (&volume);
            endpointVolume->GetChannelCount (&channels);
            endpointVolume->GetMute (&mute);
            endpointVolume->Release ();

            std::printf ("GLOBAL %f\n", volume);
        }


        UINT numberOfDevices;
        deviceCollection->GetCount (&numberOfDevices);

        for (UINT i = 0; i != numberOfDevices; ++i) {
            IAudioSessionEnumerator * sessionEnumerator = NULL;

            // GetSessionEnumerator (&deviceCollection, &sessionEnumerator, i);
            {
                IMMDevice * device;
                deviceCollection->Item (i, &device);

                IAudioSessionManager2 * audioSessionManager;
                device->Activate (__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, reinterpret_cast<void **>(&audioSessionManager));
                device->Release ();
                audioSessionManager->GetSessionEnumerator (&sessionEnumerator);
                audioSessionManager->Release ();
            }

            int numberOfSessions;
            sessionEnumerator->GetCount (&numberOfSessions);

            for (int i = 0; i < numberOfSessions; i++) {
                IAudioSessionControl2 * session = NULL;
                // GetSession (&sessionEnumerator, &session, i);
                {
                    IAudioSessionControl * ctrl1 = NULL;
                    sessionEnumerator->GetSession (i, &ctrl1);
                    ctrl1->QueryInterface (&session);
                    ctrl1->Release ();
                }

                //session->RegisterAudioSessionNotification ();

                DWORD pid = 0;
                std::string name;

                // GetProcessNameFromSession (&session, &name);
                {
                    HRESULT res = session->IsSystemSoundsSession ();
                    if (res == S_OK) {
                        // system
                        // std::printf ("system");

                    } else {
                        session->GetProcessId (&pid);

                        std::printf ("%6u", pid);
                    }
                }

                if (pid) {

                    AudioSessionState state;
                    session->GetState (&state);
                    std::printf (" [%u]: ", (unsigned) state);

                    ISimpleAudioVolume * simpleVolume = NULL;
                    if (session->QueryInterface (&simpleVolume) == S_OK) {
                        // GetSimpleVolume (&session, &simpleVolume);

                        float volume;
                        simpleVolume->GetMasterVolume (&volume);

                        BOOL mute;
                        simpleVolume->GetMute (&mute);

                        simpleVolume->Release ();

                        // session->Release ();
                        // sessionEnumerator->Release ();
                        // deviceCollection->Release ();

                        std::printf ("%.2f %s", volume, mute ? "muted" : "    ");
                    } else {
                        std::printf ("- - - - - ");
                    }

                    //GetGroupingParam

                    /* LPWSTR nnn = NULL;
                    if (session->GetDisplayName (&nnn) == S_OK) {
                        std::printf (" // %ls", nnn);
                        CoTaskMemFree (nnn);
                    };
                    if (session->GetIconPath (&nnn) == S_OK) {
                        std::printf (" // %ls", nnn);
                        CoTaskMemFree (nnn);
                    };// */
                    std::printf ("\n");
                }

                session->Release ();

            }
            sessionEnumerator->Release ();
        }
        std::printf ("\n");
        Sleep (500);
    }
    deviceCollection->Release ();

    CoUninitialize ();
    return 0;
}
