#include "application.hpp"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int result=1;
    try {
        int count=0; auto** argv=CommandLineToArgvW(GetCommandLineW(),&count);
        if(!argv) spg::win_error("Command line");
        std::vector<std::wstring> args(argv+1,argv+count); LocalFree(argv);
        auto options=spg::parse_options(args);
        auto name=spg::mutex_name(options.data_dir,L"SunPack.GUI");
        spg::Handle instance(CreateMutexW(nullptr,FALSE,name.c_str()));
        if(!instance.value) spg::win_error("GUI instance");
        if(GetLastError()==ERROR_ALREADY_EXISTS) {
            EnumWindows([](HWND hwnd,LPARAM param)->BOOL {
                if(GetPropW(hwnd,reinterpret_cast<const wchar_t*>(param))) {
                    ShowWindow(hwnd,SW_RESTORE); SetForegroundWindow(hwnd); return FALSE;
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(name.c_str()));
            result=0;
        } else {
            spg::Application app(std::move(options)); result=app.run();
        }
    } catch(const std::exception& e) { MessageBoxW(nullptr,spg::wide(e.what()).c_str(),L"SunPack 管理面板",MB_ICONERROR|MB_OK); }
    if(SUCCEEDED(com)) CoUninitialize();
    return result;
}
