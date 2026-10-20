#include "desktop_launch.hpp"
#include <windows.h>
#include <shobjidl.h>
#include <stdexcept>
uint32_t launch_codex_desktop() {
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized))throw std::runtime_error("Desktop activation initialization failed.");
    IApplicationActivationManager* manager=nullptr;
    HRESULT result=CoCreateInstance(CLSID_ApplicationActivationManager,nullptr,CLSCTX_LOCAL_SERVER,IID_IApplicationActivationManager,reinterpret_cast<void**>(&manager));
    DWORD pid=0;
    if(SUCCEEDED(result)) {
        result=manager->ActivateApplication(L"OpenAI.Codex_2p2nqsd0c76g0!App",L"--remote-debugging-port=0 --remote-debugging-address=127.0.0.1",AO_NOERRORUI,&pid);
        manager->Release();
    }
    CoUninitialize();
    if(FAILED(result))throw std::runtime_error("Registered Codex desktop activation failed.");
    return pid;
}
