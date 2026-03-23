// Copyright (c) 2016 dacci.org

#include "app/application.h"

#include <atlstr.h>
#include <shellapi.h>
#include <string>

#include "ui/main_frame.h"

namespace patch_cleaner {
namespace app {

Application::Application() : message_loop_(nullptr), frame_(nullptr) {}

Application::~Application() {}

bool Application::ParseCommandLine(LPCTSTR /*command_line*/,
                                   HRESULT* result) throw() {
  *result = S_OK;

  return true;
}

HRESULT Application::PreMessageLoop(int show_mode) throw() {
  auto result = CAtlExeModuleT::PreMessageLoop(show_mode);
  if (FAILED(result)) {
    return result;
  }

  if (!AtlInitCommonControls(0xFFFF)) {  // all classes
    return S_FALSE;
  }

  message_loop_ = new CMessageLoop();
  if (message_loop_ == nullptr) {
    return S_FALSE;
  }

  frame_ = new ui::MainFrame();
  if (frame_ == nullptr) {
    return S_FALSE;
  }

  if (frame_->CreateEx() == NULL) {
    return S_FALSE;
  }

  frame_->ResizeClient(980, 680, FALSE);
  frame_->CenterWindow();
  frame_->ShowWindow(show_mode);
  frame_->UpdateWindow();

  return S_OK;
}

HRESULT Application::PostMessageLoop() throw() {
  if (frame_ != nullptr) {
    delete frame_;
    frame_ = nullptr;
  }

  if (message_loop_ != nullptr) {
    delete message_loop_;
    message_loop_ = nullptr;
  }

  return CAtlExeModuleT::PostMessageLoop();
}

void Application::RunMessageLoop() throw() {
  message_loop_->Run();
}

}  // namespace app
}  // namespace patch_cleaner

namespace {

bool InitializeProcessSecurity() {
  if (!HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, nullptr, 0)) {
    return false;
  }

  if (!SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE |
                         BASE_SEARCH_PATH_PERMANENT)) {
    return false;
  }

  if (!SetDllDirectory(L"")) {
    return false;
  }

  return true;
}

int ShowFatalStartupError(DWORD error) {
  CString message;
  message.Format(L"Patch Cleaner could not enable required process hardening "
                 L"(error %lu).",
                 error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
  MessageBox(nullptr, message, L"Patch Cleaner", MB_ICONERROR | MB_OK);
  return static_cast<int>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

int RunElevatedOperationWithSeh(const wchar_t* request_path) {
  __try {
    return patch_cleaner::ui::RunElevatedOperationRequest(request_path);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return static_cast<int>(GetExceptionCode());
  }
}

int RunApplicationMain(int show_mode) {
  return patch_cleaner::app::Application().WinMain(show_mode);
}

int RunApplicationWithSeh(int show_mode) {
  __try {
    return RunApplicationMain(show_mode);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return static_cast<int>(GetExceptionCode());
  }
}

}  // namespace

int __stdcall wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                       wchar_t* /*command_line*/, int show_mode) {
  if (!InitializeProcessSecurity()) {
    return ShowFatalStartupError(GetLastError());
  }

#ifdef _DEBUG
  {
    auto flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    flags |=
        _CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_LEAK_CHECK_DF;
    _CrtSetDbgFlag(flags);
  }
#endif

  int argc = 0;
  auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv != nullptr) {
    const bool is_elevated_operation =
        argc == 3 && wcscmp(argv[1], L"--elevated-operation") == 0;
    if (is_elevated_operation) {
      const std::wstring request_path = argv[2];
      LocalFree(argv);
      return RunElevatedOperationWithSeh(request_path.c_str());
    }

    LocalFree(argv);
  }

  return RunApplicationWithSeh(show_mode);
}
