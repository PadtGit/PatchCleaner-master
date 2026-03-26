// Copyright (c) 2016 dacci.org

#include "ui/main_frame.h"

#include <atlstr.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <msi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <map>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "app/application.h"

namespace patch_cleaner {
namespace ui {

namespace {

typedef CWinTraitsOR<LVS_REPORT | LVS_SHOWSELALWAYS> FileListWinTraits;

struct FileItem {
  std::wstring path;
  uint64_t size;
};

struct StringLessIgnoreCase {
  bool operator()(const std::wstring& a, const std::wstring& b) const {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  }
};

typedef std::map<std::wstring, uint64_t, StringLessIgnoreCase> FileSizeMap;

enum class MutationOperation {
  kMoveToTemp,
  kDelete,
};

struct MutationReply {
  bool destination_error = false;
  bool completed = false;
  std::vector<std::wstring> succeeded_paths;
  std::vector<std::wstring> failed_paths;
};

struct LocalMemDeleter {
  void operator()(void* value) const {
    if (value != nullptr) {
      LocalFree(value);
    }
  }
};

bool BuildSecureDirectoryAttributes(
    SECURITY_ATTRIBUTES* attributes,
    std::unique_ptr<void, LocalMemDeleter>* descriptor_holder);
bool GetExecutablePath(std::wstring* executable_path);
bool OpenValidatedDirectory(const std::wstring& path, DWORD desired_access,
                            CHandle* directory_handle);

constexpr wchar_t kAllUsersSid[] = L"s-1-1-0";
constexpr wchar_t kTempMoveDirectory[] = L"C:\\TempPatchCleanerFiles";
constexpr wchar_t kOperationArgument[] = L"--elevated-operation";
constexpr wchar_t kOperationRootSuffix[] = L"\\PatchCleaner\\Operations";
constexpr wchar_t kOperationMoveToken[] = L"move";
constexpr wchar_t kOperationDeleteToken[] = L"delete";
constexpr wchar_t kResultSuccessPrefix[] = L"ok\t";
constexpr wchar_t kResultFailurePrefix[] = L"fail\t";
constexpr wchar_t kResultDestinationError[] = L"destination_error";
constexpr wchar_t kResultCompleted[] = L"completed";
constexpr wchar_t kSecureSubdirectorySddl[] =
    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
constexpr size_t kMaxFailureSamples = 5;
constexpr UINT kDefaultDpi = 96;
constexpr DWORD kResponsivePumpStride = 32;

COLORREF BlendColor(COLORREF from, COLORREF to, int percent) {
  percent = std::max(0, std::min(100, percent));
  const auto blend_channel = [&](BYTE left, BYTE right) {
    return static_cast<BYTE>(
        left + ((right - left) * percent + (right > left ? 50 : -50)) / 100);
  };

  return RGB(blend_channel(GetRValue(from), GetRValue(to)),
             blend_channel(GetGValue(from), GetGValue(to)),
             blend_channel(GetBValue(from), GetBValue(to)));
}

UINT GetWindowDpi(HWND window) {
  if (window != nullptr) {
    const auto dpi = GetDpiForWindow(window);
    if (dpi != 0) {
      return dpi;
    }
  }

  return kDefaultDpi;
}

bool IsProcessElevated() {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return false;
  }

  CHandle token_handle(raw_token);
  TOKEN_ELEVATION elevation{};
  DWORD bytes_returned = 0;
  return GetTokenInformation(token_handle, TokenElevation, &elevation,
                             sizeof(elevation), &bytes_returned) != FALSE &&
         elevation.TokenIsElevated != 0;
}

int ScaleForDpi(UINT dpi, int value) {
  return MulDiv(value, static_cast<int>(dpi), static_cast<int>(kDefaultDpi));
}

CString FormatSizeString(uint64_t size) {
  static const wchar_t* kUnits[]{L"B", L"KB", L"MB", L"GB", L"TB"};

  auto scaled_size = static_cast<double>(size);
  auto unit_index = 0;
  for (; scaled_size >= 1024.0 && unit_index < _countof(kUnits) - 1;
       ++unit_index) {
    scaled_size /= 1024.0;
  }

  wchar_t buffer[32];
  swprintf_s(buffer, L"%.1lf %s", scaled_size, kUnits[unit_index]);
  return CString(buffer);
}

CString FormatClockTime(const SYSTEMTIME& time) {
  wchar_t buffer[64]{};
  if (GetTimeFormatEx(nullptr, TIME_NOSECONDS, &time, nullptr, buffer,
                      _countof(buffer)) == 0) {
    return CString(L"recently");
  }

  return CString(buffer);
}

void PumpMessageRange(UINT message) {
  MSG queued_message{};
  while (PeekMessage(&queued_message, nullptr, message, message, PM_REMOVE)) {
    TranslateMessage(&queued_message);
    DispatchMessage(&queued_message);
  }
}

void PumpResponsiveUiMessages() {
  PumpMessageRange(WM_TIMER);
  PumpMessageRange(WM_PAINT);
}

void FillRectColor(CDCHandle dc, const RECT& rect, COLORREF color) {
  dc.FillSolidRect(&rect, color);
}

void DrawRectOutline(CDCHandle dc, const RECT& rect, COLORREF color) {
  CPen pen;
  pen.CreatePen(PS_SOLID, 1, color);
  const auto old_pen = dc.SelectPen(pen);
  const auto old_brush = dc.SelectStockBrush(NULL_BRUSH);
  dc.Rectangle(&rect);
  dc.SelectBrush(old_brush);
  dc.SelectPen(old_pen);
}

void FillRoundedRect(CDCHandle dc, const RECT& rect, COLORREF fill_color,
                     COLORREF border_color, int radius) {
  CPen pen;
  pen.CreatePen(PS_SOLID, 1, border_color);
  CBrush brush;
  brush.CreateSolidBrush(fill_color);
  const auto old_pen = dc.SelectPen(pen);
  const auto old_brush = dc.SelectBrush(brush);
  dc.RoundRect(rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  dc.SelectBrush(old_brush);
  dc.SelectPen(old_pen);
}

void DrawChevron(CDCHandle dc, const RECT& rect, bool ascending,
                 COLORREF color) {
  CPen pen;
  pen.CreatePen(PS_SOLID, 2, color);
  const auto old_pen = dc.SelectPen(pen);
  const auto old_brush = dc.SelectStockBrush(NULL_BRUSH);

  const auto center_x = (rect.left + rect.right) / 2;
  const auto center_y = (rect.top + rect.bottom) / 2;
  const auto half_width =
      std::max<int>(2, static_cast<int>((rect.right - rect.left) / 4));
  const auto half_height =
      std::max<int>(2, static_cast<int>((rect.bottom - rect.top) / 4));

  if (ascending) {
    dc.MoveTo(center_x - half_width, center_y + half_height / 2);
    dc.LineTo(center_x, center_y - half_height);
    dc.LineTo(center_x + half_width, center_y + half_height / 2);
  } else {
    dc.MoveTo(center_x - half_width, center_y - half_height / 2);
    dc.LineTo(center_x, center_y + half_height);
    dc.LineTo(center_x + half_width, center_y - half_height / 2);
  }

  dc.SelectBrush(old_brush);
  dc.SelectPen(old_pen);
}

LOGFONT BuildBaseUiFont() {
  NONCLIENTMETRICS metrics{};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                           0) != FALSE) {
    return metrics.lfMessageFont;
  }

  LOGFONT font{};
  font.lfHeight = -12;
  wcscpy_s(font.lfFaceName, L"Segoe UI");
  return font;
}

const wchar_t* GetQuerySid(MSIINSTALLCONTEXT context,
                           const std::wstring& user_sid) {
  if (context == MSIINSTALLCONTEXT_MACHINE || user_sid.empty()) {
    return nullptr;
  }

  return user_sid.c_str();
}

template <typename Query>
bool QueryInstallerString(Query query, std::wstring* value) {
  std::vector<wchar_t> buffer(256);
  for (;;) {
    DWORD length = static_cast<DWORD>(buffer.size());
    auto error = query(buffer.data(), &length);
    if (error == ERROR_SUCCESS) {
      if (length == 0) {
        value->clear();
      } else {
        value->assign(buffer.data(), length);
      }
      return true;
    }

    if (error != ERROR_MORE_DATA) {
      return false;
    }

    buffer.resize(length + 1);
  }
}

template <typename Enumerate>
UINT EnumerateWithSid(Enumerate enumerate, std::wstring* user_sid) {
  std::vector<wchar_t> buffer(256);
  for (;;) {
    DWORD length = static_cast<DWORD>(buffer.size());
    auto error = enumerate(buffer.data(), &length);
    if (error == ERROR_SUCCESS) {
      if (length == 0) {
        user_sid->clear();
      } else {
        user_sid->assign(buffer.data(), length);
      }
      return ERROR_SUCCESS;
    }

    if (error != ERROR_MORE_DATA) {
      return error;
    }

    buffer.resize(length + 1);
  }
}

std::wstring StripExtendedPathPrefix(const std::wstring& path) {
  if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    return std::wstring(L"\\\\").append(path.substr(8));
  }

  if (path.rfind(L"\\\\?\\", 0) == 0) {
    return path.substr(4);
  }

  return path;
}

std::wstring TrimTrailingSeparators(const std::wstring& path) {
  std::wstring trimmed = path;
  while (trimmed.length() > 3 &&
         (trimmed.back() == L'\\' || trimmed.back() == L'/')) {
    trimmed.pop_back();
  }

  return trimmed;
}

bool NormalizePath(const std::wstring& path, std::wstring* normalized_path) {
  DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (length == 0) {
    return false;
  }

  std::vector<wchar_t> buffer(length);
  const auto written =
      GetFullPathNameW(path.c_str(), length, buffer.data(), nullptr);
  if (written == 0 || written >= length) {
    return false;
  }

  *normalized_path =
      TrimTrailingSeparators(StripExtendedPathPrefix(std::wstring(buffer.data(),
                                                                 written)));
  return true;
}

std::wstring EnsureTrailingSeparator(const std::wstring& path) {
  if (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
    return path;
  }

  return std::wstring(path).append(L"\\");
}

bool StartsWithIgnoreCase(const std::wstring& value,
                          const std::wstring& prefix) {
  return value.length() >= prefix.length() &&
         _wcsnicmp(value.c_str(), prefix.c_str(), prefix.length()) == 0;
}

bool EndsWithIgnoreCase(const std::wstring& value, const wchar_t* suffix) {
  const auto suffix_length = wcslen(suffix);
  if (value.length() < suffix_length) {
    return false;
  }

  return _wcsicmp(value.c_str() + value.length() - suffix_length, suffix) == 0;
}

bool GetKnownFolderSubdirectory(REFKNOWNFOLDERID folder_id,
                                const wchar_t* suffix,
                                std::wstring* path) {
  PWSTR folder = nullptr;
  const auto result =
      SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &folder);
  if (FAILED(result)) {
    return false;
  }

  path->assign(folder);
  CoTaskMemFree(folder);
  path->append(suffix);
  return true;
}

bool GetInstallerDirectory(std::wstring* installer_directory) {
  if (!GetKnownFolderSubdirectory(FOLDERID_Windows, L"\\Installer",
                                  installer_directory)) {
    return false;
  }

  std::wstring normalized_path;
  if (!NormalizePath(*installer_directory, &normalized_path)) {
    return false;
  }

  *installer_directory = EnsureTrailingSeparator(normalized_path);
  return true;
}

bool GetOperationDirectory(std::wstring* operation_directory) {
  std::wstring path;
  if (!GetKnownFolderSubdirectory(FOLDERID_LocalAppData, kOperationRootSuffix,
                                  &path)) {
    return false;
  }

  return NormalizePath(path, operation_directory);
}

bool EnsureDirectoryChainExists(const std::wstring& path) {
  const auto result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
  return result == ERROR_SUCCESS || result == ERROR_FILE_EXISTS ||
         result == ERROR_ALREADY_EXISTS;
}

bool GenerateRandomHexString(size_t byte_count, std::wstring* hex_string) {
  std::vector<UCHAR> bytes(byte_count);
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(),
                                      static_cast<ULONG>(bytes.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return false;
  }

  static const wchar_t kHexDigits[] = L"0123456789abcdef";
  std::wstring output;
  output.reserve(bytes.size() * 2);
  for (auto value : bytes) {
    output.push_back(kHexDigits[value >> 4]);
    output.push_back(kHexDigits[value & 0x0F]);
  }

  *hex_string = std::move(output);
  return true;
}

bool WriteWideLinesFile(const std::wstring& path,
                        const std::vector<std::wstring>& lines,
                        bool fail_if_exists) {
  const DWORD disposition = fail_if_exists ? CREATE_NEW : CREATE_ALWAYS;
  CHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  std::wstring content;
  for (const auto& line : lines) {
    content.append(line).append(L"\n");
  }

  const auto size_in_bytes =
      static_cast<DWORD>(content.size() * sizeof(wchar_t));
  DWORD written = 0;
  return WriteFile(file, content.data(), size_in_bytes, &written, nullptr) !=
             FALSE &&
         written == size_in_bytes &&
         FlushFileBuffers(file) != FALSE;
}

bool ReadWideLinesFile(const std::wstring& path,
                       std::vector<std::wstring>* lines) {
  CHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0 ||
      file_size.QuadPart > MAXDWORD ||
      (file_size.QuadPart % sizeof(wchar_t)) != 0) {
    return false;
  }

  std::vector<wchar_t> buffer(static_cast<size_t>(file_size.QuadPart /
                                                  sizeof(wchar_t)) +
                              1);
  DWORD read = 0;
  if (file_size.QuadPart != 0 &&
      (ReadFile(file, buffer.data(), static_cast<DWORD>(file_size.QuadPart),
                &read, nullptr) == FALSE ||
       read != static_cast<DWORD>(file_size.QuadPart))) {
    return false;
  }

  buffer[read / sizeof(wchar_t)] = L'\0';

  lines->clear();
  std::wstring current_line;
  for (const auto ch : buffer) {
    if (ch == L'\0') {
      break;
    }

    if (ch == L'\r') {
      continue;
    }

    if (ch == L'\n') {
      lines->push_back(current_line);
      current_line.clear();
      continue;
    }

    current_line.push_back(ch);
  }

  if (!current_line.empty()) {
    lines->push_back(current_line);
  }

  return true;
}

const wchar_t* GetOperationToken(MutationOperation operation) {
  return operation == MutationOperation::kMoveToTemp ? kOperationMoveToken
                                                     : kOperationDeleteToken;
}

bool ParseOperationToken(const std::wstring& token,
                         MutationOperation* operation) {
  if (_wcsicmp(token.c_str(), kOperationMoveToken) == 0) {
    *operation = MutationOperation::kMoveToTemp;
    return true;
  }

  if (_wcsicmp(token.c_str(), kOperationDeleteToken) == 0) {
    *operation = MutationOperation::kDelete;
    return true;
  }

  return false;
}

std::wstring BuildResultPath(const std::wstring& request_path) {
  return request_path + L".result";
}

std::wstring BuildProgressPath(const std::wstring& request_path) {
  return request_path + L".progress";
}

bool WriteOperationRequest(MutationOperation operation,
                           const std::vector<std::wstring>& paths,
                           std::wstring* request_path,
                           std::wstring* result_path) {
  std::wstring operation_directory;
  if (!GetOperationDirectory(&operation_directory) ||
      !EnsureDirectoryChainExists(operation_directory)) {
    return false;
  }

  CHandle operation_directory_handle;
  if (!OpenValidatedDirectory(operation_directory, FILE_GENERIC_READ,
                              &operation_directory_handle)) {
    return false;
  }

  for (int attempt = 0; attempt < 8; ++attempt) {
    std::wstring random_suffix;
    if (!GenerateRandomHexString(16, &random_suffix)) {
      return false;
    }

    std::wstring candidate_path = operation_directory;
    candidate_path.append(L"\\").append(random_suffix).append(L".request");
    std::vector<std::wstring> lines;
    lines.push_back(GetOperationToken(operation));
    lines.insert(lines.end(), paths.begin(), paths.end());
    if (!WriteWideLinesFile(candidate_path, lines, true)) {
      if (GetLastError() == ERROR_FILE_EXISTS) {
        continue;
      }
      return false;
    }

    *request_path = candidate_path;
    *result_path = BuildResultPath(candidate_path);
    return true;
  }

  return false;
}

bool ReadOperationRequest(const std::wstring& request_path,
                          MutationOperation* operation,
                          std::vector<std::wstring>* paths) {
  std::vector<std::wstring> lines;
  if (!ReadWideLinesFile(request_path, &lines) || lines.empty() ||
      !ParseOperationToken(lines.front(), operation)) {
    return false;
  }

  paths->assign(lines.begin() + 1, lines.end());
  return true;
}

bool WriteOperationReply(const std::wstring& result_path,
                         const MutationReply& reply) {
  std::vector<std::wstring> lines;
  if (reply.destination_error) {
    lines.push_back(kResultDestinationError);
  }

  for (const auto& path : reply.succeeded_paths) {
    lines.emplace_back(std::wstring(kResultSuccessPrefix).append(path));
  }

  for (const auto& path : reply.failed_paths) {
    lines.emplace_back(std::wstring(kResultFailurePrefix).append(path));
  }

  if (reply.completed) {
    lines.push_back(kResultCompleted);
  }

  return WriteWideLinesFile(result_path, lines, false);
}

bool ReadOperationReply(const std::wstring& result_path, MutationReply* reply) {
  std::vector<std::wstring> lines;
  if (!ReadWideLinesFile(result_path, &lines)) {
    return false;
  }

  reply->destination_error = false;
  reply->completed = false;
  reply->succeeded_paths.clear();
  reply->failed_paths.clear();

  for (const auto& line : lines) {
    if (_wcsicmp(line.c_str(), kResultDestinationError) == 0) {
      reply->destination_error = true;
      continue;
    }

    if (StartsWithIgnoreCase(line, kResultSuccessPrefix)) {
      reply->succeeded_paths.push_back(
          line.substr(wcslen(kResultSuccessPrefix)));
      continue;
    }

    if (StartsWithIgnoreCase(line, kResultFailurePrefix)) {
      reply->failed_paths.push_back(line.substr(wcslen(kResultFailurePrefix)));
      continue;
    }

    if (_wcsicmp(line.c_str(), kResultCompleted) == 0) {
      reply->completed = true;
    }
  }

  return true;
}

bool RelaunchApplicationElevated(HWND owner_window) {
  std::wstring executable_path;
  if (!GetExecutablePath(&executable_path)) {
    return false;
  }

  SHELLEXECUTEINFOW execute_info{};
  execute_info.cbSize = sizeof(execute_info);
  execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
  execute_info.hwnd = owner_window;
  execute_info.lpVerb = L"runas";
  execute_info.lpFile = executable_path.c_str();
  execute_info.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&execute_info)) {
    return false;
  }

  if (execute_info.hProcess != nullptr) {
    CloseHandle(execute_info.hProcess);
  }

  return true;
}

bool GetExecutablePath(std::wstring* executable_path) {
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  for (;;) {
    const auto written =
        GetModuleFileNameW(nullptr, buffer.data(),
                           static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return false;
    }

    if (written < static_cast<DWORD>(buffer.size() - 1)) {
      executable_path->assign(buffer.data(), written);
      return true;
    }

    buffer.resize(buffer.size() * 2);
  }
}

bool LaunchElevatedOperation(MutationOperation operation,
                             const std::vector<std::wstring>& paths,
                             MutationReply* reply, bool* canceled) {
  *canceled = false;

  std::wstring request_path;
  std::wstring result_path;
  if (!WriteOperationRequest(operation, paths, &request_path, &result_path)) {
    return false;
  }
  const auto progress_path = BuildProgressPath(request_path);

  const auto cleanup = [&]() {
    DeleteFileW(progress_path.c_str());
    DeleteFileW(result_path.c_str());
    DeleteFileW(request_path.c_str());
  };

  std::wstring executable_path;
  if (!GetExecutablePath(&executable_path)) {
    cleanup();
    return false;
  }

  std::wstring parameters(kOperationArgument);
  parameters.append(L" \"").append(request_path).append(L"\"");

  SHELLEXECUTEINFOW execute_info{};
  execute_info.cbSize = sizeof(execute_info);
  execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
  execute_info.lpVerb = L"runas";
  execute_info.lpFile = executable_path.c_str();
  execute_info.lpParameters = parameters.c_str();
  execute_info.nShow = SW_HIDE;

  if (!ShellExecuteExW(&execute_info)) {
    const auto error = GetLastError();
    cleanup();
    if (error == ERROR_CANCELLED) {
      *canceled = true;
    }
    return false;
  }

  HANDLE process_handle = execute_info.hProcess;
  for (;;) {
    const auto wait_result =
        MsgWaitForMultipleObjects(1, &process_handle, FALSE, 50,
                                  QS_PAINT | QS_TIMER);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }

    if (wait_result == WAIT_OBJECT_0 + 1 || wait_result == WAIT_TIMEOUT) {
      PumpResponsiveUiMessages();
      continue;
    }

    CloseHandle(execute_info.hProcess);
    cleanup();
    return false;
  }

  CloseHandle(execute_info.hProcess);

  auto read_success = ReadOperationReply(result_path, reply);
  if (!read_success) {
    read_success = ReadOperationReply(progress_path, reply);
  }
  cleanup();
  return read_success;
}

bool GetPathFromHandle(HANDLE handle, std::wstring* path) {
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  for (;;) {
    const auto length =
        GetFinalPathNameByHandleW(handle, buffer.data(),
                                  static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) {
      return false;
    }

    if (length < static_cast<DWORD>(buffer.size())) {
      *path = TrimTrailingSeparators(StripExtendedPathPrefix(
          std::wstring(buffer.data(), length)));
      return true;
    }

    buffer.resize(length + 1);
  }
}

bool GetVolumeRootPath(const std::wstring& path, std::wstring* volume_root) {
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  if (!GetVolumePathNameW(path.c_str(), buffer.data(),
                          static_cast<DWORD>(buffer.size()))) {
    return false;
  }

  *volume_root = TrimTrailingSeparators(std::wstring(buffer.data()));
  return true;
}

bool ValidateDirectoryHandle(HANDLE handle, const std::wstring& expected_path) {
  FILE_STANDARD_INFO standard_info{};
  if (!GetFileInformationByHandleEx(handle, FileStandardInfo, &standard_info,
                                    sizeof(standard_info)) ||
      !standard_info.Directory) {
    return false;
  }

  FILE_ATTRIBUTE_TAG_INFO attribute_info{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                    &attribute_info, sizeof(attribute_info)) ||
      attribute_info.FileAttributes == INVALID_FILE_ATTRIBUTES ||
      (attribute_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return false;
  }

  std::wstring final_path;
  return GetPathFromHandle(handle, &final_path) &&
         _wcsicmp(final_path.c_str(), expected_path.c_str()) == 0;
}

bool OpenValidatedDirectory(const std::wstring& path, DWORD desired_access,
                            CHandle* directory_handle) {
  std::wstring normalized_path;
  if (!NormalizePath(path, &normalized_path)) {
    return false;
  }

  CHandle handle(CreateFileW(path.c_str(), desired_access,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             nullptr));
  if (handle == INVALID_HANDLE_VALUE ||
      !ValidateDirectoryHandle(handle, normalized_path)) {
    return false;
  }

  directory_handle->Attach(handle.Detach());
  return true;
}

bool EnsureValidatedMoveRoot(CHandle* root_directory_handle) {
  if (!CreateDirectoryW(kTempMoveDirectory, nullptr)) {
    const auto error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      return false;
    }
  }

  if (!OpenValidatedDirectory(kTempMoveDirectory,
                              FILE_GENERIC_READ | READ_CONTROL | WRITE_DAC,
                              root_directory_handle)) {
    return false;
  }

  std::unique_ptr<void, LocalMemDeleter> descriptor_holder;
  SECURITY_ATTRIBUTES attributes{};
  if (!BuildSecureDirectoryAttributes(&attributes, &descriptor_holder)) {
    return false;
  }

  PACL dacl = nullptr;
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  if (!GetSecurityDescriptorDacl(attributes.lpSecurityDescriptor, &dacl_present,
                                 &dacl, &dacl_defaulted) ||
      !dacl_present || dacl == nullptr) {
    return false;
  }

  if (SetSecurityInfo(*root_directory_handle, SE_FILE_OBJECT,
                      DACL_SECURITY_INFORMATION |
                          PROTECTED_DACL_SECURITY_INFORMATION,
                      nullptr, nullptr, dacl, nullptr) != ERROR_SUCCESS) {
    return false;
  }

  return true;
}

bool BuildSecureDirectoryAttributes(SECURITY_ATTRIBUTES* attributes,
                                    std::unique_ptr<void, LocalMemDeleter>*
                                        descriptor_holder) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kSecureSubdirectorySddl, SDDL_REVISION_1, &descriptor, nullptr)) {
    return false;
  }

  descriptor_holder->reset(descriptor);
  attributes->nLength = sizeof(*attributes);
  attributes->lpSecurityDescriptor = descriptor;
  attributes->bInheritHandle = FALSE;
  return true;
}

bool IsInstallerCacheExtension(const std::wstring& path) {
  return EndsWithIgnoreCase(path, L".msi") || EndsWithIgnoreCase(path, L".msp");
}

bool IsInstallerCachePath(const std::wstring& path,
                          const std::wstring& installer_directory) {
  std::wstring normalized_path;
  return NormalizePath(path, &normalized_path) &&
         StartsWithIgnoreCase(normalized_path, installer_directory) &&
         IsInstallerCacheExtension(normalized_path);
}

bool OpenValidatedInstallerFile(const std::wstring& path,
                                const std::wstring& installer_directory,
                                CHandle* file_handle) {
  if (!IsInstallerCachePath(path, installer_directory)) {
    return false;
  }

  CHandle handle(CreateFileW(path.c_str(),
                             DELETE | FILE_READ_ATTRIBUTES |
                                 FILE_WRITE_ATTRIBUTES | GENERIC_READ |
                                 SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  FILE_STANDARD_INFO standard_info{};
  if (!GetFileInformationByHandleEx(handle, FileStandardInfo, &standard_info,
                                    sizeof(standard_info)) ||
      standard_info.Directory) {
    return false;
  }

  FILE_ATTRIBUTE_TAG_INFO attribute_info{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                    &attribute_info, sizeof(attribute_info)) ||
      (attribute_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return false;
  }

  std::wstring final_path;
  if (!GetPathFromHandle(handle, &final_path) ||
      !StartsWithIgnoreCase(final_path, installer_directory) ||
      !IsInstallerCacheExtension(final_path)) {
    return false;
  }

  file_handle->Attach(handle.Detach());
  return true;
}

bool PrepareFileForMutation(HANDLE handle, FILE_BASIC_INFO* original_info,
                            bool* attributes_changed) {
  if (!GetFileInformationByHandleEx(handle, FileBasicInfo, original_info,
                                    sizeof(*original_info))) {
    return false;
  }

  *attributes_changed =
      (original_info->FileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  if (!*attributes_changed) {
    return true;
  }

  FILE_BASIC_INFO writable_info = *original_info;
  writable_info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
  return SetFileInformationByHandle(handle, FileBasicInfo, &writable_info,
                                    sizeof(writable_info)) != FALSE;
}

void RestoreFileAttributesIfNeeded(HANDLE handle,
                                   const FILE_BASIC_INFO& original_info,
                                   bool attributes_changed) {
  if (attributes_changed) {
    auto restored_info = original_info;
    SetFileInformationByHandle(handle, FileBasicInfo, &restored_info,
                               sizeof(restored_info));
  }
}

bool DeleteOpenedFile(HANDLE handle) {
  FILE_DISPOSITION_INFO disposition_info{};
  disposition_info.DeleteFile = TRUE;
  return SetFileInformationByHandle(handle, FileDispositionInfo,
                                    &disposition_info,
                                    sizeof(disposition_info)) != FALSE;
}

bool DeleteInstallerCacheFile(const std::wstring& path,
                              const std::wstring& installer_directory) {
  CHandle file_handle;
  FILE_BASIC_INFO original_info{};
  auto attributes_changed = false;
  if (!OpenValidatedInstallerFile(path, installer_directory, &file_handle) ||
      !PrepareFileForMutation(file_handle, &original_info,
                              &attributes_changed)) {
    return false;
  }

  if (DeleteOpenedFile(file_handle)) {
    return true;
  }

  RestoreFileAttributesIfNeeded(file_handle, original_info, attributes_changed);
  return false;
}

std::wstring GetFileNameFromPath(const std::wstring& path) {
  const auto separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return path;
  }

  return path.substr(separator + 1);
}

std::wstring BuildMoveCandidateName(const std::wstring& file_name,
                                    const std::wstring& batch_token,
                                    int suffix) {
  const auto extension = file_name.find_last_of(L'.');
  std::wstring base_name = file_name;
  std::wstring extension_name;
  if (extension != std::wstring::npos) {
    base_name = file_name.substr(0, extension);
    extension_name = file_name.substr(extension);
  }

  std::wstring candidate_name = base_name;
  if (!batch_token.empty()) {
    candidate_name.append(L" [").append(batch_token).append(L"]");
  }

  if (suffix > 0) {
    CString suffix_text;
    suffix_text.Format(L" (%d)", suffix);
    candidate_name.append(suffix_text.GetString());
  }

  return candidate_name + extension_name;
}

bool CheckMoveNameAvailability(const std::wstring& destination_directory,
                               const std::wstring& candidate_name,
                               bool* available) {
  auto candidate_path = destination_directory;
  candidate_path.append(L"\\").append(candidate_name);

  const auto attributes = GetFileAttributesW(candidate_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      *available = true;
      return true;
    }
    return false;
  }

  *available = false;
  return true;
}

bool BuildUniqueMoveName(const std::wstring& destination_directory,
                         const std::wstring& source_path,
                         const std::wstring& batch_token,
                         std::wstring* destination_name) {
  const auto file_name = GetFileNameFromPath(source_path);
  auto available = false;
  if (!CheckMoveNameAvailability(destination_directory, file_name, &available)) {
    return false;
  }
  if (available) {
    *destination_name = file_name;
    return true;
  }

  // Use a per-operation token before probing suffixes so old temp-root
  // contents do not force an ever-growing linear scan.
  for (int suffix = 0;; ++suffix) {
    auto candidate_name = BuildMoveCandidateName(file_name, batch_token, suffix);
    if (!CheckMoveNameAvailability(destination_directory, candidate_name,
                                   &available)) {
      return false;
    }
    if (available) {
      *destination_name = std::move(candidate_name);
      return true;
    }
  }
}

bool RenameOpenedFileIntoDirectory(HANDLE file_handle,
                                   HANDLE destination_directory_handle,
                                   const std::wstring& destination_name) {
  const auto name_bytes =
      static_cast<DWORD>(destination_name.size() * sizeof(wchar_t));
  std::vector<BYTE> rename_buffer(FIELD_OFFSET(FILE_RENAME_INFO, FileName) +
                                  name_bytes);
  auto* rename_info =
      reinterpret_cast<FILE_RENAME_INFO*>(rename_buffer.data());
  rename_info->ReplaceIfExists = FALSE;
  rename_info->RootDirectory = destination_directory_handle;
  rename_info->FileNameLength = name_bytes;
  memcpy(rename_info->FileName, destination_name.data(), name_bytes);

  return SetFileInformationByHandle(file_handle, FileRenameInfo, rename_info,
                                    static_cast<DWORD>(rename_buffer.size())) !=
         FALSE;
}

bool CopyOpenedFileToDestination(HANDLE source_handle,
                                 const std::wstring& destination_path) {
  CHandle destination_handle(CreateFileW(destination_path.c_str(), GENERIC_WRITE,
                                         0, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
  if (destination_handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(source_handle, zero, nullptr, FILE_BEGIN)) {
    return false;
  }

  std::vector<BYTE> buffer(64 * 1024);
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(source_handle, buffer.data(),
                  static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      return false;
    }

    if (read == 0) {
      return FlushFileBuffers(destination_handle) != FALSE;
    }

    DWORD written = 0;
    if (!WriteFile(destination_handle, buffer.data(), read, &written,
                   nullptr) ||
        written != read) {
      return false;
    }
  }
}

bool CopyAndDeleteOpenedFile(HANDLE source_handle,
                             const std::wstring& destination_path,
                             const FILE_BASIC_INFO& original_info,
                             bool attributes_changed) {
  if (CopyOpenedFileToDestination(source_handle, destination_path) &&
      DeleteOpenedFile(source_handle)) {
    return true;
  }

  DeleteFileW(destination_path.c_str());
  RestoreFileAttributesIfNeeded(source_handle, original_info,
                                attributes_changed);
  return false;
}

bool MoveInstallerCacheFile(const std::wstring& path,
                            const std::wstring& installer_directory,
                            const std::wstring& destination_directory,
                            HANDLE destination_directory_handle,
                            const std::wstring& batch_token) {
  CHandle file_handle;
  FILE_BASIC_INFO original_info{};
  auto attributes_changed = false;
  if (!OpenValidatedInstallerFile(path, installer_directory, &file_handle) ||
      !PrepareFileForMutation(file_handle, &original_info,
                              &attributes_changed)) {
    return false;
  }

  std::wstring destination_name;
  if (!BuildUniqueMoveName(destination_directory, path, batch_token,
                           &destination_name)) {
    RestoreFileAttributesIfNeeded(file_handle, original_info,
                                  attributes_changed);
    return false;
  }

  std::wstring final_source_path;
  if (!GetPathFromHandle(file_handle, &final_source_path)) {
    RestoreFileAttributesIfNeeded(file_handle, original_info,
                                  attributes_changed);
    return false;
  }

  std::wstring destination_path(destination_directory);
  destination_path.append(L"\\").append(destination_name);

  std::wstring source_volume_root;
  std::wstring destination_volume_root;
  const auto same_volume =
      GetVolumeRootPath(final_source_path, &source_volume_root) &&
      GetVolumeRootPath(destination_path, &destination_volume_root) &&
      _wcsicmp(source_volume_root.c_str(), destination_volume_root.c_str()) ==
          0;
  if (!same_volume) {
    return CopyAndDeleteOpenedFile(file_handle, destination_path, original_info,
                                   attributes_changed);
  }

  if (RenameOpenedFileIntoDirectory(file_handle, destination_directory_handle,
                                    destination_name)) {
    return true;
  }

  // Some installer cache files reject the in-place rename even after elevation.
  // Fall back to a validated copy-and-delete so Move to Temp still succeeds.
  return CopyAndDeleteOpenedFile(file_handle, destination_path, original_info,
                                 attributes_changed);
}

bool ExecuteDeleteOperation(const std::vector<std::wstring>& paths,
                            const std::wstring& progress_path,
                            MutationReply* reply) {
  std::wstring installer_directory;
  if (!GetInstallerDirectory(&installer_directory)) {
    return false;
  }

  for (const auto& path : paths) {
    if (DeleteInstallerCacheFile(path, installer_directory)) {
      reply->succeeded_paths.push_back(path);
    } else {
      reply->failed_paths.push_back(path);
    }

    WriteOperationReply(progress_path, *reply);
  }

  return true;
}

bool ExecuteMoveOperation(const std::vector<std::wstring>& paths,
                          const std::wstring& progress_path,
                          MutationReply* reply) {
  std::wstring installer_directory;
  if (!GetInstallerDirectory(&installer_directory)) {
    return false;
  }

  CHandle root_directory_handle;
  if (!EnsureValidatedMoveRoot(&root_directory_handle)) {
    reply->destination_error = true;
    return false;
  }

  std::wstring batch_token;
  if (!GenerateRandomHexString(8, &batch_token)) {
    return false;
  }

  for (const auto& path : paths) {
    if (MoveInstallerCacheFile(path, installer_directory, kTempMoveDirectory,
                               root_directory_handle, batch_token)) {
      reply->succeeded_paths.push_back(path);
    } else {
      reply->failed_paths.push_back(path);
    }

    WriteOperationReply(progress_path, *reply);
  }

  return true;
}

void RecordFailurePath(const std::wstring& path, int* failure_count,
                       std::vector<std::wstring>* failure_samples) {
  ++(*failure_count);
  if (failure_samples->size() < kMaxFailureSamples) {
    failure_samples->push_back(path);
  }
}

CString BuildFailureMessage(const wchar_t* operation, int failure_count,
                            const std::vector<std::wstring>& failure_samples) {
  CString message;
  message.Format(L"%s failed for %d selected file%s.", operation, failure_count,
                 failure_count == 1 ? L"" : L"s");

  if (!failure_samples.empty()) {
    message.Append(L"\n\nExamples:");
    for (const auto& path : failure_samples) {
      message.Append(L"\n - ");
      message.Append(path.c_str());
    }

    const auto remaining =
        failure_count - static_cast<int>(failure_samples.size());
    if (remaining > 0) {
      CString remaining_text;
      remaining_text.Format(L"\n - ...and %d more.", remaining);
      message.Append(remaining_text);
    }
  }

  return message;
}

template <size_t length>
int FormatSize(uint64_t size, wchar_t (&buffer)[length]) {
  static const wchar_t* kUnits[]{L"B", L"KB", L"MB", L"GB", L"TB"};

  auto double_size = static_cast<double>(size);
  auto index = 0;
  for (; double_size >= 1024.0 && index < _countof(kUnits) - 1; ++index) {
    double_size /= 1024.0;
  }

  return swprintf_s(buffer, L"%.1lf %s", double_size, kUnits[index]);
}

void EnumFiles(const std::wstring& base_path, const wchar_t* pattern,
               FileSizeMap* output) {
  auto query = base_path + pattern;
  WIN32_FIND_DATAW find_data{};
  auto find = FindFirstFileW(query.c_str(), &find_data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD enumerated = 0;
  do {
    if ((find_data.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      continue;
    }

    auto path = base_path + find_data.cFileName;

    ULARGE_INTEGER size{};
    size.LowPart = find_data.nFileSizeLow;
    size.HighPart = find_data.nFileSizeHigh;

    output->insert({path, size.QuadPart});

    ++enumerated;
    if ((enumerated % kResponsivePumpStride) == 0) {
      PumpResponsiveUiMessages();
    }
  } while (FindNextFileW(find, &find_data));

  FindClose(find);
}

bool RemoveInstalledPackages(FileSizeMap* files) {
  for (DWORD index = 0;; ++index) {
    wchar_t product_code[39]{};
    MSIINSTALLCONTEXT context = MSIINSTALLCONTEXT_NONE;
    std::wstring user_sid;
    auto error = EnumerateWithSid(
        [&](wchar_t* sid_buffer, DWORD* sid_length) {
          return MsiEnumProductsEx(nullptr, kAllUsersSid, MSIINSTALLCONTEXT_ALL,
                                   index, product_code, &context, sid_buffer,
                                   sid_length);
        },
        &user_sid);
    if (error == ERROR_NO_MORE_ITEMS) {
      break;
    }

    if (error != ERROR_SUCCESS) {
      return false;
    }

    std::wstring local_package;
    if (!QueryInstallerString(
            [&](wchar_t* buffer, DWORD* length) {
              return MsiGetProductInfoEx(
                  product_code, GetQuerySid(context, user_sid), context,
                  INSTALLPROPERTY_LOCALPACKAGE, buffer, length);
            },
            &local_package)) {
      return false;
    }

    files->erase(local_package);
    if ((index % kResponsivePumpStride) == 0) {
      PumpResponsiveUiMessages();
    }
  }

  return true;
}

bool RemoveInstalledPatches(FileSizeMap* files) {
  for (DWORD index = 0;; ++index) {
    wchar_t patch_code[39]{};
    wchar_t product_code[39]{};
    MSIINSTALLCONTEXT context = MSIINSTALLCONTEXT_NONE;
    std::wstring user_sid;
    auto error = EnumerateWithSid(
        [&](wchar_t* sid_buffer, DWORD* sid_length) {
          return MsiEnumPatchesEx(nullptr, kAllUsersSid, MSIINSTALLCONTEXT_ALL,
                                  MSIPATCHSTATE_ALL, index, patch_code,
                                  product_code, &context, sid_buffer,
                                  sid_length);
        },
        &user_sid);
    if (error == ERROR_NO_MORE_ITEMS) {
      break;
    }

    if (error != ERROR_SUCCESS) {
      return false;
    }

    std::wstring local_package;
    if (!QueryInstallerString(
            [&](wchar_t* buffer, DWORD* length) {
              return MsiGetPatchInfoEx(
                  patch_code, product_code, GetQuerySid(context, user_sid),
                  context, INSTALLPROPERTY_LOCALPACKAGE, buffer, length);
            },
            &local_package)) {
      return false;
    }

    files->erase(local_package);
    if ((index % kResponsivePumpStride) == 0) {
      PumpResponsiveUiMessages();
    }
  }

  return true;
}

constexpr COLORREF kWindowBackground = RGB(243, 238, 232);
constexpr COLORREF kCommandBandBackground = RGB(249, 246, 241);
constexpr COLORREF kListFrameBackground = RGB(255, 253, 250);
constexpr COLORREF kActionRailBackground = RGB(247, 243, 238);
constexpr COLORREF kPanelBorder = RGB(219, 209, 198);
constexpr COLORREF kDividerColor = RGB(227, 218, 208);
constexpr COLORREF kInkColor = RGB(38, 34, 30);
constexpr COLORREF kMutedInkColor = RGB(111, 99, 87);
constexpr COLORREF kAccentColor = RGB(173, 110, 66);
constexpr COLORREF kAccentDarkColor = RGB(144, 90, 55);
constexpr COLORREF kDangerColor = RGB(145, 67, 58);
constexpr COLORREF kDisabledInkColor = RGB(155, 145, 137);
constexpr COLORREF kDisabledFillColor = RGB(233, 227, 220);
constexpr COLORREF kButtonTextOnAccent = RGB(252, 248, 242);
constexpr COLORREF kListStripeColor = RGB(252, 250, 247);
constexpr COLORREF kListSelectedColor = RGB(242, 228, 216);

bool StepAnimationToward(int* value, int target, int step) {
  if (*value == target) {
    return false;
  }

  if (*value < target) {
    *value = std::min(target, *value + step);
  } else {
    *value = std::max(target, *value - step);
  }

  return true;
}

bool DecayFlashValue(int* value, int amount) {
  if (*value == 0) {
    return false;
  }

  *value = std::max(0, *value - amount);
  return true;
}

}  // namespace

MainFrame::MainFrame()
    : dpi_(kDefaultDpi),
      selected_size_(0),
      moved_size_(0),
      deleted_size_(0),
      total_reclaimable_size_(0),
      selected_count_(0),
      total_reclaimable_count_(0),
      sort_column_(0),
      hot_button_(0),
      pressed_button_(0),
      reveal_progress_(0),
      action_progress_(0),
      selected_flash_(0),
      moved_flash_(0),
      deleted_flash_(0),
      scan_flash_(0),
      cached_path_column_width_(0),
      cached_size_column_width_(0),
      sort_ascending_(true),
      batching_selection_changes_(false),
      pending_selection_refresh_(false),
      tracking_mouse_(false),
      has_last_scan_(false),
      last_scan_succeeded_(false),
      recovered_last_operation_(false),
      busy_operation_(BusyOperation::kNone) {
  ZeroMemory(&last_scan_time_, sizeof(last_scan_time_));
}

BOOL MainFrame::PreTranslateMessage(MSG* message) {
  if (CFrameWindowImpl::PreTranslateMessage(message)) {
    return TRUE;
  }

  return FALSE;
}

int MainFrame::OnCreate(CREATESTRUCT* /*create*/) {
  dpi_ = GetWindowDpi(m_hWnd);
  ModifyStyle(0, WS_CLIPCHILDREN);

  if (!app::GetApplication()->GetMessageLoop()->AddMessageFilter(this)) {
    return -1;
  }

  RebuildFonts();

  m_hWndClient = file_list_.Create(
      m_hWnd, nullptr, nullptr, FileListWinTraits::GetWndStyle(0),
      FileListWinTraits::GetWndExStyle(0), IDC_FILE_LIST);
  if (!file_list_.IsWindow()) {
    return -1;
  }

  file_list_.ModifyStyleEx(WS_EX_CLIENTEDGE, 0);
  file_list_.SetExtendedListViewStyle(
      LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
      LVS_EX_SIMPLESELECT | LVS_EX_AUTOSIZECOLUMNS);

  LVCOLUMN column{LVCF_FMT | LVCF_WIDTH | LVCF_TEXT};
  column.fmt = LVCFMT_LEFT;
  column.cx = ScaleForDpi(dpi_, 250);
  column.pszText = L"Path";
  file_list_.InsertColumn(0, &column);

  column.fmt = LVCFMT_RIGHT;
  column.cx = ScaleForDpi(dpi_, 104);
  column.pszText = L"Size";
  file_list_.InsertColumn(1, &column);

  SyncListAppearance();
  RefreshChrome(false, true);
  BeginSettleAnimation();

  return 0;
}

void MainFrame::OnDestroy() {
  KillTimer(kAnimationTimerId);
  if (GetCapture() == m_hWnd) {
    ReleaseCapture();
  }

  SetMsgHandled(FALSE);

  app::GetApplication()->GetMessageLoop()->RemoveMessageFilter(this);
  DisposeFonts();
}

void MainFrame::OnSize(UINT /*type*/, CSize /*size*/) {
  LayoutChildren();
  Invalidate(FALSE);
}

void MainFrame::OnPaint(CDCHandle dc) {
  CDCHandle target_dc(dc);
  if (target_dc == nullptr) {
    CPaintDC paint_dc(m_hWnd);
    OnPaint(paint_dc.m_hDC);
    return;
  }

  CRect client_rect;
  GetClientRect(&client_rect);
  if (client_rect.IsRectEmpty()) {
    return;
  }

  CDC memory_dc;
  memory_dc.CreateCompatibleDC(target_dc);

  CBitmap buffer_bitmap;
  buffer_bitmap.CreateCompatibleBitmap(target_dc, client_rect.Width(),
                                       client_rect.Height());
  const auto old_bitmap = memory_dc.SelectBitmap(buffer_bitmap);

  PaintChrome(memory_dc.m_hDC);

  target_dc.BitBlt(0, 0, client_rect.Width(), client_rect.Height(), memory_dc,
                   0, 0, SRCCOPY);
  memory_dc.SelectBitmap(old_bitmap);
}

BOOL MainFrame::OnEraseBkgnd(CDCHandle /*dc*/) {
  return TRUE;
}

void MainFrame::OnMouseMove(UINT /*flags*/, CPoint point) {
  if (!tracking_mouse_) {
    TRACKMOUSEEVENT track_event{};
    track_event.cbSize = sizeof(track_event);
    track_event.dwFlags = TME_LEAVE;
    track_event.hwndTrack = m_hWnd;
    tracking_mouse_ = TrackMouseEvent(&track_event) != FALSE;
  }

  UpdateHotButton(GetButtonAtPoint(point));
}

void MainFrame::OnMouseLeave() {
  tracking_mouse_ = false;
  UpdateHotButton(0);
}

void MainFrame::OnLButtonDown(UINT /*flags*/, CPoint point) {
  const auto command_id = GetButtonAtPoint(point);
  if (command_id == 0) {
    SetMsgHandled(FALSE);
    return;
  }

  pressed_button_ = command_id;
  SetCapture();
  InvalidateRect(GetButtonRect(command_id), FALSE);
}

void MainFrame::OnLButtonUp(UINT /*flags*/, CPoint point) {
  const auto was_pressed = pressed_button_;
  if (GetCapture() == m_hWnd) {
    ReleaseCapture();
  }

  pressed_button_ = 0;
  if (was_pressed != 0) {
    InvalidateRect(GetButtonRect(was_pressed), FALSE);
    if (was_pressed == GetButtonAtPoint(point)) {
      InvokeSurfaceButton(was_pressed);
    }
  }
}

void MainFrame::OnTimer(UINT_PTR event_id) {
  if (event_id != kAnimationTimerId) {
    return;
  }

  const auto action_target = selected_count_ > 0 ? 100 : 0;
  const auto reveal_changed = StepAnimationToward(&reveal_progress_, 100, 18);
  const auto action_changed =
      StepAnimationToward(&action_progress_, action_target, 20);
  const auto selected_flash_changed = DecayFlashValue(&selected_flash_, 12);
  const auto moved_flash_changed = DecayFlashValue(&moved_flash_, 10);
  const auto deleted_flash_changed = DecayFlashValue(&deleted_flash_, 10);
  const auto scan_flash_changed = DecayFlashValue(&scan_flash_, 10);
  const auto animating =
      reveal_changed || action_changed || selected_flash_changed ||
      moved_flash_changed || deleted_flash_changed || scan_flash_changed;

  if (!animating) {
    KillTimer(kAnimationTimerId);
  }

  if (reveal_changed || action_changed) {
    LayoutChildren();
    Invalidate(FALSE);
    return;
  }

  if (scan_flash_changed) {
    InvalidateRect(command_band_rect_, FALSE);
    InvalidateRect(list_frame_rect_, FALSE);
  }
  if (selected_flash_changed || moved_flash_changed || deleted_flash_changed) {
    InvalidateRect(action_rail_rect_, FALSE);
  }
}

void MainFrame::OnSetFocus(CWindow /*old_window*/) {
  if (file_list_.IsWindow()) {
    file_list_.SetFocus();
  }
}

void MainFrame::OnGetMinMaxInfo(MINMAXINFO* min_max_info) {
  min_max_info->ptMinTrackSize.x = ScaleForDpi(dpi_, 820);
  min_max_info->ptMinTrackSize.y = ScaleForDpi(dpi_, 560);
}

LRESULT MainFrame::OnItemChanged(NMHDR* header) {
  auto data = reinterpret_cast<NMLISTVIEW*>(header);
  if (data->iItem < 0) {
    return 0;
  }

  if (data->uChanged & LVIF_STATE) {
    auto old_checked =
        (data->uOldState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
    auto new_checked =
        (data->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);

    if (old_checked != new_checked) {
      auto* file_item =
          reinterpret_cast<FileItem*>(file_list_.GetItemData(data->iItem));
      if (file_item == nullptr) {
        return 0;
      }

      if (new_checked) {
        selected_size_ += file_item->size;
        ++selected_count_;
      } else {
        selected_size_ -= file_item->size;
        selected_count_ = std::max(0, selected_count_ - 1);
      }

      if (batching_selection_changes_) {
        pending_selection_refresh_ = true;
      } else {
        RefreshChrome(true);
      }
    }
  }

  return 0;
}

LRESULT MainFrame::OnColumnClick(NMHDR* header) {
  auto data = reinterpret_cast<NMLISTVIEW*>(header);
  if (data->iSubItem == sort_column_) {
    sort_ascending_ = !sort_ascending_;
  } else {
    sort_column_ = data->iSubItem;
    sort_ascending_ = true;
  }

  ApplySort();
  InvalidateRect(list_frame_rect_, FALSE);
  return 0;
}

LRESULT MainFrame::OnDeleteItem(NMHDR* header) {
  auto data = reinterpret_cast<NMLISTVIEW*>(header);
  delete reinterpret_cast<FileItem*>(data->lParam);

  return 0;
}

void MainFrame::OnFileUpdate(UINT /*notify_code*/, int /*id*/,
                             CWindow /*control*/) {
  if (IsBusy()) {
    return;
  }

  if (!IsProcessElevated()) {
    const auto response = MessageBox(
        L"Patch Cleaner needs administrator access to scan the Windows "
        L"Installer cache safely.\n\nRelaunch as administrator now?",
        L"Patch Cleaner", MB_ICONQUESTION | MB_YESNO);
    if (response == IDYES) {
      if (RelaunchApplicationElevated(m_hWnd)) {
        PostMessage(WM_CLOSE);
      } else if (GetLastError() != ERROR_CANCELLED) {
        MessageBox(L"Patch Cleaner could not relaunch itself with "
                   L"administrator rights.",
                   L"Patch Cleaner", MB_ICONERROR | MB_OK);
      }
    }
    return;
  }

  SetBusyOperation(BusyOperation::kScanning);
  recovered_last_operation_ = false;
  InvalidateRect(command_band_rect_, FALSE);
  InvalidateRect(action_rail_rect_, FALSE);

  std::wstring cache_path;
  if (!GetInstallerDirectory(&cache_path)) {
    ClearBusyOperation();
    MessageBox(L"Patch Cleaner could not resolve the Windows Installer cache.",
               L"Patch Cleaner", MB_ICONERROR | MB_OK);
    return;
  }

  FileSizeMap files;
  EnumFiles(cache_path, L"*.msi", &files);
  EnumFiles(cache_path, L"*.msp", &files);
  const auto scan_complete =
      RemoveInstalledPackages(&files) && RemoveInstalledPatches(&files);
  if (!scan_complete) {
    files.clear();
  }

  total_reclaimable_count_ = static_cast<int>(files.size());
  total_reclaimable_size_ = 0;
  for (const auto& pair : files) {
    total_reclaimable_size_ += pair.second;
  }

  file_list_.SetRedraw(FALSE);
  file_list_.DeleteAllItems();

  auto count = 0;
  LVITEM item{};
  wchar_t size_text[32];
  for (auto& pair : files) {
    auto* file_item = new FileItem{pair.first, pair.second};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = count;
    item.iSubItem = 0;
    item.pszText = const_cast<wchar_t*>(file_item->path.c_str());
    item.lParam = reinterpret_cast<LPARAM>(file_item);
    item.iItem = file_list_.InsertItem(&item);
    if (item.iItem == -1) {
      delete file_item;
      break;
    }

    FormatSize(file_item->size, size_text);

    item.mask = LVIF_TEXT;
    item.iSubItem = 1;
    item.pszText = size_text;
    if (file_list_.SetItem(&item) == -1) {
      break;
    }

    ++count;
    if ((count % static_cast<int>(kResponsivePumpStride)) == 0) {
      PumpResponsiveUiMessages();
    }
  }

  GetLocalTime(&last_scan_time_);
  has_last_scan_ = true;
  last_scan_succeeded_ = scan_complete;
  selected_size_ = 0;
  selected_count_ = 0;
  ApplySort();
  scan_flash_ = 100;
  BeginSettleAnimation();
  RefreshChrome(false, true);

  file_list_.SetRedraw(TRUE);
  file_list_.RedrawWindow(
      NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
  ClearBusyOperation();

  if (!scan_complete) {
    MessageBox(L"Patch Cleaner could not complete the installer scan safely, "
               L"so no files were listed.",
               L"Patch Cleaner", MB_ICONERROR | MB_OK);
  }
}

void MainFrame::OnEditSelectAll(UINT /*notify_code*/, int /*id*/,
                                CWindow /*control*/) {
  if (IsBusy() || file_list_.GetItemCount() == 0) {
    return;
  }

  batching_selection_changes_ = true;
  pending_selection_refresh_ = false;
  file_list_.SetRedraw(FALSE);
  auto any_changed = false;
  for (auto i = 0, ix = file_list_.GetItemCount(); i < ix; ++i) {
    if (file_list_.GetCheckState(i) == FALSE) {
      any_changed = true;
      file_list_.SetCheckState(i, TRUE);
    }
  }
  file_list_.SetRedraw(TRUE);
  file_list_.RedrawWindow(
      NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
  batching_selection_changes_ = false;
  RecalculateSelectionTotals();
  if (pending_selection_refresh_ || any_changed) {
    pending_selection_refresh_ = false;
    RefreshChrome(true);
  }
}

void MainFrame::OnFileMoveToTemp(UINT /*notify_code*/, int /*id*/,
                                 CWindow /*control*/) {
  if (IsBusy()) {
    return;
  }

  std::vector<std::wstring> paths;
  FileSizeMap selected_sizes;
  for (auto i = 0, ix = file_list_.GetItemCount(); i < ix; ++i) {
    auto state = file_list_.GetItemState(i, LVIS_STATEIMAGEMASK);
    if ((state & INDEXTOSTATEIMAGEMASK(2)) == 0) {
      continue;
    }

    auto* file_item =
        reinterpret_cast<FileItem*>(file_list_.GetItemData(i));
    if (file_item == nullptr) {
      continue;
    }

    paths.push_back(file_item->path);
    selected_sizes.insert({file_item->path, file_item->size});
  }

  if (paths.empty()) {
    return;
  }

  SetBusyOperation(BusyOperation::kMoveToTemp);
  recovered_last_operation_ = false;
  InvalidateRect(command_band_rect_, FALSE);
  InvalidateRect(action_rail_rect_, FALSE);

  MutationReply reply;
  bool canceled = false;
  if (!LaunchElevatedOperation(MutationOperation::kMoveToTemp, paths, &reply,
                               &canceled)) {
    ClearBusyOperation();
    if (!canceled) {
      MessageBox(L"Patch Cleaner could not complete the elevated move request.",
                 L"Patch Cleaner", MB_ICONERROR | MB_OK);
      PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
    }
    return;
  }

  uint64_t moved_this_run = 0;
  for (const auto& path : reply.succeeded_paths) {
    auto found = selected_sizes.find(path);
    if (found != selected_sizes.end()) {
      moved_this_run += found->second;
    }
  }

  moved_size_ += moved_this_run;
  if (moved_this_run > 0) {
    moved_flash_ = 100;
  }
  recovered_last_operation_ = !reply.completed;
  PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
  RefreshChrome();
  ClearBusyOperation();

  if (reply.destination_error) {
    MessageBox(L"Could not access C:\\TempPatchCleanerFiles securely.",
               L"Patch Cleaner", MB_ICONERROR | MB_OK);
  }

  if (!reply.completed) {
    MessageBox(L"Move to Temp ended unexpectedly. Patch Cleaner recovered the "
               L"partial results it could and started a fresh scan.",
               L"Patch Cleaner", MB_ICONWARNING | MB_OK);
  }

  if (!reply.failed_paths.empty()) {
    int move_failure_count = 0;
    std::vector<std::wstring> move_failure_samples;
    for (const auto& path : reply.failed_paths) {
      RecordFailurePath(path, &move_failure_count, &move_failure_samples);
    }

    auto message =
        BuildFailureMessage(L"Move to Temp", move_failure_count,
                            move_failure_samples);
    MessageBox(message, L"Patch Cleaner", MB_ICONWARNING | MB_OK);
  }
}

void MainFrame::OnEditDelete(UINT /*notify_code*/, int /*id*/,
                             CWindow /*control*/) {
  if (IsBusy()) {
    return;
  }

  std::vector<std::wstring> paths;
  FileSizeMap selected_sizes;
  for (auto i = 0, ix = file_list_.GetItemCount(); i < ix; ++i) {
    auto state = file_list_.GetItemState(i, LVIS_STATEIMAGEMASK);
    if ((state & INDEXTOSTATEIMAGEMASK(2)) == 0) {
      continue;
    }

    auto* file_item =
        reinterpret_cast<FileItem*>(file_list_.GetItemData(i));
    if (file_item == nullptr) {
      continue;
    }

    paths.push_back(file_item->path);
    selected_sizes.insert({file_item->path, file_item->size});
  }

  if (paths.empty()) {
    return;
  }

  SetBusyOperation(BusyOperation::kDelete);
  recovered_last_operation_ = false;
  InvalidateRect(command_band_rect_, FALSE);
  InvalidateRect(action_rail_rect_, FALSE);

  MutationReply reply;
  bool canceled = false;
  if (!LaunchElevatedOperation(MutationOperation::kDelete, paths, &reply,
                               &canceled)) {
    ClearBusyOperation();
    if (!canceled) {
      MessageBox(L"Patch Cleaner could not complete the elevated delete "
                 L"request.",
                 L"Patch Cleaner", MB_ICONERROR | MB_OK);
      PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
    }
    return;
  }

  uint64_t deleted_this_run = 0;
  for (const auto& path : reply.succeeded_paths) {
    auto found = selected_sizes.find(path);
    if (found != selected_sizes.end()) {
      deleted_this_run += found->second;
    }
  }

  deleted_size_ += deleted_this_run;
  if (deleted_this_run > 0) {
    deleted_flash_ = 100;
  }
  recovered_last_operation_ = !reply.completed;
  PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
  RefreshChrome();
  ClearBusyOperation();

  if (!reply.completed) {
    MessageBox(L"Delete ended unexpectedly. Patch Cleaner recovered the "
               L"partial results it could and started a fresh scan.",
               L"Patch Cleaner", MB_ICONWARNING | MB_OK);
  }

  if (!reply.failed_paths.empty()) {
    int delete_failure_count = 0;
    std::vector<std::wstring> delete_failure_samples;
    for (const auto& path : reply.failed_paths) {
      RecordFailurePath(path, &delete_failure_count, &delete_failure_samples);
    }

    auto message =
        BuildFailureMessage(L"Delete", delete_failure_count,
                            delete_failure_samples);
    MessageBox(message, L"Patch Cleaner", MB_ICONWARNING | MB_OK);
  }
}

void MainFrame::ApplySort() {
  if (file_list_.GetItemCount() > 1) {
    file_list_.SortItems(CompareFileItems, reinterpret_cast<LPARAM>(this));
  }
}

void MainFrame::RecalculateSelectionTotals() {
  selected_size_ = 0;
  selected_count_ = 0;
  for (auto i = 0, ix = file_list_.GetItemCount(); i < ix; ++i) {
    if (file_list_.GetCheckState(i) == FALSE) {
      continue;
    }

    const auto* file_item =
        reinterpret_cast<const FileItem*>(file_list_.GetItemData(i));
    if (file_item == nullptr) {
      continue;
    }

    selected_size_ += file_item->size;
    ++selected_count_;
  }
}

void MainFrame::SetBusyOperation(BusyOperation operation) {
  if (busy_operation_ == operation) {
    return;
  }

  busy_operation_ = operation;
  if (file_list_.IsWindow()) {
    file_list_.EnableWindow(!IsBusy());
  }

  if (IsBusy()) {
    UpdateHotButton(0);
    if (GetCapture() == m_hWnd) {
      ReleaseCapture();
    }
    pressed_button_ = 0;
  }

  Invalidate(FALSE);
}

void MainFrame::ClearBusyOperation() {
  SetBusyOperation(BusyOperation::kNone);
}

bool MainFrame::IsBusy() const {
  return busy_operation_ != BusyOperation::kNone;
}

void MainFrame::RebuildFonts() {
  DisposeFonts();

  const auto base_font = BuildBaseUiFont();
  const auto create_font = [&](CFont* font, int point_size, LONG weight) {
    LOGFONT face = base_font;
    face.lfHeight = -MulDiv(point_size, static_cast<int>(dpi_), 72);
    face.lfWeight = weight;
    face.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    font->CreateFontIndirect(&face);
  };

  create_font(&headline_font_, 9, FW_SEMIBOLD);
  create_font(&title_font_, 24, FW_SEMIBOLD);
  create_font(&body_font_, 10, FW_NORMAL);
  create_font(&caption_font_, 9, FW_NORMAL);
  create_font(&button_font_, 10, FW_SEMIBOLD);
}

void MainFrame::DisposeFonts() {
  if (!headline_font_.IsNull()) {
    headline_font_.DeleteObject();
  }
  if (!title_font_.IsNull()) {
    title_font_.DeleteObject();
  }
  if (!body_font_.IsNull()) {
    body_font_.DeleteObject();
  }
  if (!caption_font_.IsNull()) {
    caption_font_.DeleteObject();
  }
  if (!button_font_.IsNull()) {
    button_font_.DeleteObject();
  }
}

void MainFrame::SyncListAppearance() {
  if (!file_list_.IsWindow()) {
    return;
  }

  file_list_.SetBkColor(kListFrameBackground);
  file_list_.SetTextBkColor(kListFrameBackground);
  file_list_.SetTextColor(kInkColor);
  file_list_.SetFont(body_font_);

  CHeaderCtrl header(file_list_.GetHeader());
  if (header.IsWindow()) {
    header.SetFont(button_font_);
  }
}

void MainFrame::LayoutChildren() {
  if (!file_list_.IsWindow()) {
    return;
  }

  CRect client_rect;
  GetClientRect(&client_rect);
  if (client_rect.IsRectEmpty()) {
    return;
  }

  const auto outer = ScaleForDpi(dpi_, 20);
  const auto gap = ScaleForDpi(dpi_, 14);
  const auto command_height = ScaleForDpi(dpi_, 182);
  const auto action_height = ScaleForDpi(dpi_, 96) +
                             MulDiv(ScaleForDpi(dpi_, 34), action_progress_, 100);
  const auto settle_offset =
      MulDiv(ScaleForDpi(dpi_, 12), 100 - reveal_progress_, 100);

  command_band_rect_.SetRect(outer, outer, client_rect.right - outer,
                             outer + command_height);
  action_rail_rect_.SetRect(outer, client_rect.bottom - outer - action_height,
                            client_rect.right - outer, client_rect.bottom - outer);
  list_frame_rect_.SetRect(outer, command_band_rect_.bottom + gap,
                           client_rect.right - outer, action_rail_rect_.top - gap);

  const auto button_height = ScaleForDpi(dpi_, 42);
  const auto button_gap = ScaleForDpi(dpi_, 10);
  const auto button_padding_right = ScaleForDpi(dpi_, 24);
  const auto scan_width = ScaleForDpi(dpi_, 148);
  const auto scan_top = command_band_rect_.top + ScaleForDpi(dpi_, 42) +
                        settle_offset / 3;
  scan_button_rect_.SetRect(command_band_rect_.right - button_padding_right -
                                scan_width,
                            scan_top,
                            command_band_rect_.right - button_padding_right,
                            scan_top + button_height);

  const auto delete_width = ScaleForDpi(dpi_, 114);
  const auto move_width = ScaleForDpi(dpi_, 156);
  const auto select_width = ScaleForDpi(dpi_, 122);
  const auto button_top = action_rail_rect_.bottom - ScaleForDpi(dpi_, 22) -
                          button_height;

  delete_button_rect_.SetRect(action_rail_rect_.right - button_padding_right -
                                  delete_width,
                              button_top,
                              action_rail_rect_.right - button_padding_right,
                              button_top + button_height);
  move_to_temp_button_rect_.SetRect(delete_button_rect_.left - button_gap -
                                        move_width,
                                    button_top,
                                    delete_button_rect_.left - button_gap,
                                    button_top + button_height);
  select_all_button_rect_.SetRect(move_to_temp_button_rect_.left - button_gap -
                                      select_width,
                                  button_top,
                                  move_to_temp_button_rect_.left - button_gap,
                                  button_top + button_height);

  auto list_rect = list_frame_rect_;
  list_rect.DeflateRect(1, 1);
  list_rect.top += settle_offset;
  file_list_.SetWindowPos(nullptr, list_rect.left, list_rect.top,
                          list_rect.Width(), list_rect.Height(),
                          SWP_NOACTIVATE | SWP_NOZORDER);
  UpdateListColumns();
}

void MainFrame::UpdateListColumns() {
  if (!file_list_.IsWindow()) {
    return;
  }

  CRect list_rect;
  file_list_.GetClientRect(&list_rect);
  if (list_rect.IsRectEmpty()) {
    return;
  }

  const auto size_column_width = ScaleForDpi(dpi_, 112);
  const auto path_column_width =
      std::max(ScaleForDpi(dpi_, 240),
               list_rect.Width() - size_column_width - ScaleForDpi(dpi_, 28));
  if (cached_path_column_width_ != path_column_width) {
    file_list_.SetColumnWidth(0, path_column_width);
    cached_path_column_width_ = path_column_width;
  }
  if (cached_size_column_width_ != size_column_width) {
    file_list_.SetColumnWidth(1, size_column_width);
    cached_size_column_width_ = size_column_width;
  }
}

void MainFrame::RefreshChrome(bool pulse_selection, bool relayout) {
  if (pulse_selection) {
    selected_flash_ = 100;
  }

  if (relayout) {
    LayoutChildren();
  }
  EnsureAnimationTimer();
  if (relayout) {
    Invalidate(FALSE);
  } else {
    InvalidateRect(action_rail_rect_, FALSE);
  }
}

void MainFrame::BeginSettleAnimation() {
  reveal_progress_ = 0;
  EnsureAnimationTimer();
}

void MainFrame::EnsureAnimationTimer() {
  const auto action_target = selected_count_ > 0 ? 100 : 0;
  const auto should_run = reveal_progress_ < 100 ||
                          action_progress_ != action_target ||
                          selected_flash_ > 0 || moved_flash_ > 0 ||
                          deleted_flash_ > 0 || scan_flash_ > 0;
  if (should_run) {
    SetTimer(kAnimationTimerId, 15);
  } else {
    KillTimer(kAnimationTimerId);
  }
}

int MainFrame::GetButtonAtPoint(CPoint point) const {
  const std::array<int, 4> buttons{{
      kButtonScan,
      kButtonSelectAll,
      kButtonMoveToTemp,
      kButtonDelete,
  }};

  for (const auto command_id : buttons) {
    if (IsButtonEnabled(command_id) &&
        GetButtonRect(command_id).PtInRect(point)) {
      return command_id;
    }
  }

  return 0;
}

CRect MainFrame::GetButtonRect(int command_id) const {
  switch (command_id) {
    case kButtonScan:
      return scan_button_rect_;
    case kButtonSelectAll:
      return select_all_button_rect_;
    case kButtonMoveToTemp:
      return move_to_temp_button_rect_;
    case kButtonDelete:
      return delete_button_rect_;
    default:
      return CRect();
  }
}

bool MainFrame::IsButtonEnabled(int command_id) const {
  if (IsBusy()) {
    return false;
  }

  switch (command_id) {
    case kButtonScan:
      return true;
    case kButtonSelectAll:
      return file_list_.IsWindow() && file_list_.GetItemCount() > 0;
    case kButtonMoveToTemp:
    case kButtonDelete:
      return selected_count_ > 0;
    default:
      return false;
  }
}

void MainFrame::UpdateHotButton(int command_id) {
  if (hot_button_ == command_id) {
    return;
  }

  const auto old_button = hot_button_;
  hot_button_ = command_id;
  if (old_button != 0) {
    InvalidateRect(GetButtonRect(old_button), FALSE);
  }
  if (hot_button_ != 0) {
    InvalidateRect(GetButtonRect(hot_button_), FALSE);
  }
}

void MainFrame::InvokeSurfaceButton(int command_id) {
  if (!IsButtonEnabled(command_id)) {
    return;
  }

  SendMessage(WM_COMMAND, MAKEWPARAM(command_id, 0), 0);
}

void MainFrame::DrawSurfaceButton(CDCHandle dc, const CRect& rect,
                                  int command_id) const {
  if (rect.IsRectEmpty()) {
    return;
  }

  const auto enabled = IsButtonEnabled(command_id);
  const auto hot = enabled && hot_button_ == command_id;
  const auto pressed = enabled && pressed_button_ == command_id;
  const auto primary =
      command_id == kButtonScan || command_id == kButtonMoveToTemp;
  const auto destructive = command_id == kButtonDelete;

  COLORREF fill_color = primary ? kAccentColor : kActionRailBackground;
  COLORREF border_color = primary ? kAccentDarkColor : kPanelBorder;
  COLORREF text_color = primary ? kButtonTextOnAccent : kInkColor;
  if (!enabled) {
    fill_color = primary ? BlendColor(kDisabledFillColor, kAccentColor, 12)
                         : kDisabledFillColor;
    border_color = BlendColor(kPanelBorder, kActionRailBackground, 20);
    text_color = kDisabledInkColor;
  } else if (destructive) {
    fill_color = hot ? BlendColor(kActionRailBackground, kDangerColor, 16)
                     : BlendColor(kActionRailBackground, kDangerColor, 6);
    border_color = BlendColor(kDangerColor, kActionRailBackground,
                              hot ? 5 : 18);
    text_color = hot ? kDangerColor : BlendColor(kInkColor, kDangerColor, 70);
  } else if (!primary) {
    fill_color = hot ? BlendColor(kActionRailBackground, kAccentColor, 10)
                     : kActionRailBackground;
    border_color = hot ? BlendColor(kPanelBorder, kAccentColor, 45)
                       : kPanelBorder;
  }

  if (pressed && enabled) {
    fill_color = BlendColor(fill_color, RGB(32, 28, 24), 10);
    border_color = BlendColor(border_color, RGB(32, 28, 24), 12);
  }

  FillRoundedRect(dc, rect, fill_color, border_color, ScaleForDpi(dpi_, 14));

  CString label;
  switch (command_id) {
    case kButtonScan:
      label = L"Scan";
      break;
    case kButtonSelectAll:
      label = L"Select All";
      break;
    case kButtonMoveToTemp:
      label = L"Move to Temp";
      break;
    case kButtonDelete:
      label = L"Delete";
      break;
  }

  const auto old_font = dc.SelectFont(button_font_);
  const auto old_mode = dc.SetBkMode(TRANSPARENT);
  const auto old_color = dc.SetTextColor(text_color);
  auto text_rect = rect;
  dc.DrawText(label, label.GetLength(), &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  dc.SetTextColor(old_color);
  dc.SetBkMode(old_mode);
  dc.SelectFont(old_font);
}

void MainFrame::PaintChrome(CDCHandle dc) {
  CRect client_rect;
  GetClientRect(&client_rect);

  FillRectColor(dc, client_rect, kWindowBackground);
  PaintCommandBand(dc, command_band_rect_);
  PaintListFrame(dc, list_frame_rect_);
  PaintActionRail(dc, action_rail_rect_);
}

void MainFrame::PaintCommandBand(CDCHandle dc, const CRect& rect) const {
  FillRectColor(dc, rect, kCommandBandBackground);

  const auto old_mode = dc.SetBkMode(TRANSPARENT);
  const auto settle_offset =
      MulDiv(ScaleForDpi(dpi_, 10), 100 - reveal_progress_, 100);
  auto text_rect = rect;
  text_rect.left += ScaleForDpi(dpi_, 28);
  text_rect.top += ScaleForDpi(dpi_, 22) + settle_offset;
  text_rect.right = scan_button_rect_.left - ScaleForDpi(dpi_, 28);

  auto label_rect = text_rect;
  label_rect.bottom = label_rect.top + ScaleForDpi(dpi_, 20);
  const auto old_font = dc.SelectFont(headline_font_);
  auto old_color = dc.SetTextColor(BlendColor(kMutedInkColor, kAccentColor, 54));
  dc.DrawText(L"UTILITY MAINTENANCE", -1, &label_rect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  auto title_rect = text_rect;
  title_rect.top = label_rect.bottom + ScaleForDpi(dpi_, 4);
  title_rect.bottom = title_rect.top + ScaleForDpi(dpi_, 44);
  dc.SelectFont(title_font_);
  dc.SetTextColor(kInkColor);
  dc.DrawText(L"Patch Cleaner", -1, &title_rect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  auto body_rect = text_rect;
  body_rect.top = title_rect.bottom + ScaleForDpi(dpi_, 8);
  body_rect.bottom = body_rect.top + ScaleForDpi(dpi_, 40);
  dc.SelectFont(body_font_);
  dc.SetTextColor(kMutedInkColor);
  dc.DrawText(
      L"Review leftover Windows Installer packages before moving them to a "
      L"secured temp folder or deleting them.",
      -1, &body_rect, DT_LEFT | DT_WORDBREAK);

  auto state_rect = text_rect;
  state_rect.top = body_rect.bottom + ScaleForDpi(dpi_, 12);
  state_rect.bottom = state_rect.top + ScaleForDpi(dpi_, 20);
  dc.SelectFont(button_font_);
  dc.SetTextColor(BlendColor(kInkColor, kAccentColor, scan_flash_ / 3));
  auto state_line = BuildScanStateLine();
  dc.DrawText(state_line, state_line.GetLength(), &state_rect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  auto detail_rect = text_rect;
  detail_rect.top = state_rect.bottom - ScaleForDpi(dpi_, 3);
  detail_rect.bottom = detail_rect.top + ScaleForDpi(dpi_, 20);
  dc.SelectFont(caption_font_);
  dc.SetTextColor(BlendColor(kMutedInkColor, kInkColor, 18));
  auto detail_line = BuildScanDetailLine();
  dc.DrawText(detail_line, detail_line.GetLength(), &detail_rect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

  auto divider_rect = rect;
  divider_rect.top = rect.bottom - 1;
  FillRectColor(dc, divider_rect, kDividerColor);
  DrawSurfaceButton(dc, scan_button_rect_, kButtonScan);

  dc.SetTextColor(old_color);
  dc.SetBkMode(old_mode);
  dc.SelectFont(old_font);
}

void MainFrame::PaintListFrame(CDCHandle dc, const CRect& rect) const {
  FillRectColor(dc, rect, kListFrameBackground);
  DrawRectOutline(dc, rect, kPanelBorder);

  auto top_band = rect;
  top_band.bottom = rect.top + ScaleForDpi(dpi_, 3);
  FillRectColor(dc, top_band,
                BlendColor(kDividerColor, kAccentColor, 18 + scan_flash_ / 8));
}

void MainFrame::PaintActionRail(CDCHandle dc, const CRect& rect) const {
  FillRectColor(dc, rect, kActionRailBackground);

  auto top_rule = rect;
  top_rule.bottom = rect.top + 1;
  FillRectColor(dc, top_rule,
                BlendColor(kDividerColor, kAccentColor, 16 + action_progress_ / 5));

  const auto old_mode = dc.SetBkMode(TRANSPARENT);
  const auto rail_padding = ScaleForDpi(dpi_, 24);
  const auto left_width = std::max<int>(
      ScaleForDpi(dpi_, 220),
      static_cast<int>(select_all_button_rect_.left - rect.left -
                       rail_padding * 2));
  auto text_rect = rect;
  text_rect.left += rail_padding;
  text_rect.top += ScaleForDpi(dpi_, 18);
  text_rect.right = text_rect.left + left_width;

  const auto accent_flash =
      std::max(std::max(selected_flash_, moved_flash_), deleted_flash_);
  auto detail_target_color = kMutedInkColor;
  if (deleted_flash_ >= moved_flash_ && deleted_flash_ >= scan_flash_ &&
      deleted_flash_ > 0) {
    detail_target_color = kDangerColor;
  } else if (std::max(moved_flash_, scan_flash_) > 0) {
    detail_target_color = kAccentColor;
  }

  const auto old_font = dc.SelectFont(button_font_);
  auto old_color =
      dc.SetTextColor(BlendColor(kInkColor, kAccentColor, selected_flash_ / 2));
  auto state_rect = text_rect;
  state_rect.bottom = state_rect.top + ScaleForDpi(dpi_, 22);
  auto selection_line = BuildSelectionStateLine();
  dc.DrawText(selection_line, selection_line.GetLength(), &state_rect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

  auto detail_rect = text_rect;
  detail_rect.top = state_rect.bottom + ScaleForDpi(dpi_, 6) +
                    MulDiv(ScaleForDpi(dpi_, 6), action_progress_, 100);
  detail_rect.bottom = detail_rect.top + ScaleForDpi(dpi_, 20);
  dc.SelectFont(caption_font_);
  dc.SetTextColor(
      BlendColor(kActionRailBackground, detail_target_color,
                 std::max(18, accent_flash / 2 + action_progress_ / 3)));
  auto detail_line = BuildSelectionDetailLine();
  dc.DrawText(detail_line, detail_line.GetLength(), &detail_rect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

  DrawSurfaceButton(dc, select_all_button_rect_, kButtonSelectAll);
  DrawSurfaceButton(dc, move_to_temp_button_rect_, kButtonMoveToTemp);
  DrawSurfaceButton(dc, delete_button_rect_, kButtonDelete);

  dc.SetTextColor(old_color);
  dc.SetBkMode(old_mode);
  dc.SelectFont(old_font);
}

LRESULT MainFrame::PaintHeaderCustomDraw(NMCUSTOMDRAW* draw) {
  switch (draw->dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      CDCHandle dc(draw->hdc);
      CRect item_rect(draw->rc);
      const auto item_index = static_cast<int>(draw->dwItemSpec);

      auto fill_color =
          item_index == sort_column_
              ? BlendColor(kListFrameBackground, kAccentColor, 9)
              : BlendColor(kListFrameBackground, kWindowBackground, 10);
      FillRectColor(dc, item_rect, fill_color);

      auto bottom_rule = item_rect;
      bottom_rule.top = bottom_rule.bottom - 1;
      FillRectColor(dc, bottom_rule, kDividerColor);

      auto right_rule = item_rect;
      right_rule.left = right_rule.right - 1;
      FillRectColor(dc, right_rule, kDividerColor);

      wchar_t buffer[64]{};
      HDITEM item{};
      item.mask = HDI_TEXT | HDI_FORMAT;
      item.pszText = buffer;
      item.cchTextMax = _countof(buffer);
      Header_GetItem(draw->hdr.hwndFrom, item_index, &item);

      auto text_rect = item_rect;
      text_rect.DeflateRect(ScaleForDpi(dpi_, 12), 0);
      if (item_index == sort_column_) {
        const auto indicator_width = ScaleForDpi(dpi_, 18);
        auto arrow_rect = item_rect;
        arrow_rect.left =
            arrow_rect.right - indicator_width - ScaleForDpi(dpi_, 8);
        arrow_rect.right -= ScaleForDpi(dpi_, 6);
        arrow_rect.top += ScaleForDpi(dpi_, 7);
        arrow_rect.bottom -= ScaleForDpi(dpi_, 7);
        DrawChevron(dc, arrow_rect, sort_ascending_,
                    BlendColor(kInkColor, kAccentColor, 55));
        text_rect.right = arrow_rect.left - ScaleForDpi(dpi_, 6);
      }

      const auto old_font = dc.SelectFont(button_font_);
      const auto old_mode = dc.SetBkMode(TRANSPARENT);
      const auto old_color = dc.SetTextColor(kMutedInkColor);
      dc.DrawText(buffer, -1, &text_rect,
                  DT_SINGLELINE | DT_VCENTER |
                      ((item.fmt & HDF_RIGHT) != 0 ? DT_RIGHT : DT_LEFT));
      dc.SetTextColor(old_color);
      dc.SetBkMode(old_mode);
      dc.SelectFont(old_font);
      return CDRF_SKIPDEFAULT;
    }
  }

  return CDRF_DODEFAULT;
}

LRESULT MainFrame::PaintListCustomDraw(NMLVCUSTOMDRAW* draw) {
  switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const auto item_index = static_cast<int>(draw->nmcd.dwItemSpec);
      const auto selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
      const auto checked =
          item_index >= 0 && file_list_.GetCheckState(item_index) != FALSE;

      COLORREF background = (item_index % 2) == 0 ? kListFrameBackground
                                                  : kListStripeColor;
      if (checked) {
        background = BlendColor(background, kAccentColor, 5);
      }
      if (selected) {
        background = kListSelectedColor;
      }

      draw->clrTextBk = background;
      draw->clrText = kInkColor;
      return CDRF_DODEFAULT;
    }
  }

  return CDRF_DODEFAULT;
}

CString MainFrame::BuildScanStateLine() const {
  if (busy_operation_ == BusyOperation::kScanning) {
    return CString(L"Scanning Windows Installer cache...");
  }

  if (!has_last_scan_) {
    return CString(L"Ready to scan the Windows Installer cache.");
  }

  CString state_line;
  if (last_scan_succeeded_) {
    state_line.Format(L"Last scan %s | %d reclaimable file%s",
                      static_cast<LPCWSTR>(FormatClockTime(last_scan_time_)),
                      total_reclaimable_count_,
                      total_reclaimable_count_ == 1 ? L"" : L"s");
  } else {
    state_line.Format(L"Last scan %s | scan could not complete safely",
                      static_cast<LPCWSTR>(FormatClockTime(last_scan_time_)));
  }

  return state_line;
}

CString MainFrame::BuildScanDetailLine() const {
  if (busy_operation_ == BusyOperation::kScanning) {
    return CString(L"Reviewing MSI and MSP files while keeping the window "
                   L"responsive.");
  }

  if (!has_last_scan_) {
    return CString(
        L"Scan reviews MSI and MSP leftovers in the Windows Installer cache.");
  }

  if (!last_scan_succeeded_) {
    return CString(
        L"No files were listed because the scan could not complete safely.");
  }

  CString detail_line;
  detail_line.Format(L"%s reclaimable across the current scan.",
                     static_cast<LPCWSTR>(
                         FormatSizeString(total_reclaimable_size_)));
  return detail_line;
}

CString MainFrame::BuildSelectionStateLine() const {
  if (busy_operation_ == BusyOperation::kMoveToTemp) {
    return CString(L"Move to Temp is running | actions are paused");
  }

  if (busy_operation_ == BusyOperation::kDelete) {
    return CString(L"Delete is running | actions are paused");
  }

  if (busy_operation_ == BusyOperation::kScanning) {
    return CString(L"Scanning in progress | actions are temporarily disabled");
  }

  CString state_line;
  if (selected_count_ > 0) {
    state_line.Format(L"Selected %d file%s | %s chosen", selected_count_,
                      selected_count_ == 1 ? L"" : L"s",
                      static_cast<LPCWSTR>(FormatSizeString(selected_size_)));
  } else {
    state_line = L"Selected 0 files | choose installers to enable actions";
  }

  return state_line;
}

CString MainFrame::BuildSelectionDetailLine() const {
  if (busy_operation_ != BusyOperation::kNone) {
    return CString(
        L"Patch Cleaner will refresh the file list after the current task.");
  }

  if (recovered_last_operation_) {
    return CString(L"Recovered partial elevated results and refreshed the "
                   L"installer cache.");
  }

  CString detail_line;
  detail_line.Format(L"Moved %s | Deleted %s | Temp destination %s",
                     static_cast<LPCWSTR>(FormatSizeString(moved_size_)),
                     static_cast<LPCWSTR>(FormatSizeString(deleted_size_)),
                     kTempMoveDirectory);
  return detail_line;
}

LRESULT MainFrame::OnCustomDraw(NMHDR* header) {
  if (header->hwndFrom == file_list_.m_hWnd) {
    return PaintListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(header));
  }

  CHeaderCtrl list_header(file_list_.GetHeader());
  if (list_header.IsWindow() && header->hwndFrom == list_header.m_hWnd) {
    return PaintHeaderCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(header));
  }

  SetMsgHandled(FALSE);
  return 0;
}

LRESULT MainFrame::OnDpiChanged(UINT /*message*/, WPARAM w_param, LPARAM l_param,
                                BOOL& handled) {
  handled = TRUE;
  dpi_ = HIWORD(w_param);
  RebuildFonts();
  SyncListAppearance();

  const auto* suggested_rect = reinterpret_cast<const RECT*>(l_param);
  SetWindowPos(nullptr, suggested_rect->left, suggested_rect->top,
               suggested_rect->right - suggested_rect->left,
               suggested_rect->bottom - suggested_rect->top,
               SWP_NOACTIVATE | SWP_NOZORDER);

  LayoutChildren();
  Invalidate(FALSE);
  return 0;
}

int CALLBACK MainFrame::CompareFileItems(LPARAM left, LPARAM right,
                                         LPARAM context) {
  const auto* frame = reinterpret_cast<MainFrame*>(context);
  const auto* left_item = reinterpret_cast<const FileItem*>(left);
  const auto* right_item = reinterpret_cast<const FileItem*>(right);
  if (frame == nullptr || left_item == nullptr || right_item == nullptr) {
    return 0;
  }

  int result = 0;
  if (frame->sort_column_ == 1) {
    if (left_item->size < right_item->size) {
      result = -1;
    } else if (left_item->size > right_item->size) {
      result = 1;
    } else {
      result = _wcsicmp(left_item->path.c_str(), right_item->path.c_str());
    }
  } else {
    result = _wcsicmp(left_item->path.c_str(), right_item->path.c_str());
  }

  return frame->sort_ascending_ ? result : -result;
}

int RunElevatedOperationRequest(const wchar_t* request_path) {
  MutationOperation operation = MutationOperation::kDelete;
  std::vector<std::wstring> paths;
  if (request_path == nullptr ||
      !ReadOperationRequest(request_path, &operation, &paths)) {
    return ERROR_INVALID_DATA;
  }

  const auto progress_path = BuildProgressPath(request_path);
  MutationReply reply;
  const auto success =
      operation == MutationOperation::kMoveToTemp
          ? ExecuteMoveOperation(paths, progress_path, &reply)
          : ExecuteDeleteOperation(paths, progress_path, &reply);
  reply.completed = success;
  WriteOperationReply(progress_path, reply);

  if (!WriteOperationReply(BuildResultPath(request_path), reply)) {
    return GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT
                                           : static_cast<int>(GetLastError());
  }

  return success ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

}  // namespace ui
}  // namespace patch_cleaner
