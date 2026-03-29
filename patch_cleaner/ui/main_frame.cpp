// Copyright (c) 2016 dacci.org

#include "ui/main_frame.h"

#include <atlstr.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <msi.h>
#include <msidefs.h>
#include <msiquery.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <map>
#include <cstring>
#include <cwctype>
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

struct AppSettings {
  bool deep_scan_enabled = true;
  bool missing_files_check_on_startup = false;
  std::vector<std::wstring> exclusion_filters;
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
bool EnsureDirectoryChainExists(const std::wstring& path);
bool GetExecutablePath(std::wstring* executable_path);
bool OpenValidatedDirectory(const std::wstring& path, DWORD desired_access,
                            CHandle* directory_handle);

constexpr wchar_t kAllUsersSid[] = L"s-1-1-0";
constexpr wchar_t kTempMoveDirectory[] = L"C:\\TempPatchCleanerFiles";
constexpr wchar_t kOperationArgument[] = L"--elevated-operation";
constexpr wchar_t kAppDataRootSuffix[] = L"\\PatchCleaner";
constexpr wchar_t kOperationRootSuffix[] = L"\\PatchCleaner\\Operations";
constexpr wchar_t kSettingsFileName[] = L"settings.ini";
constexpr wchar_t kShareMetricsFileName[] = L"share_metrics.tsv";
constexpr wchar_t kShareProductUrl[] =
    L"https://github.com/PadtGit/PatchCleaner-master";
constexpr wchar_t kOperationMoveToken[] = L"move";
constexpr wchar_t kOperationDeleteToken[] = L"delete";
constexpr wchar_t kShareEventClicked[] = L"share_summary_clicked";
constexpr wchar_t kShareEventCopied[] = L"share_summary_copied";
constexpr wchar_t kShareEventCopyFailed[] = L"share_summary_copy_failed";
constexpr wchar_t kResultSuccessPrefix[] = L"ok\t";
constexpr wchar_t kResultFailurePrefix[] = L"fail\t";
constexpr wchar_t kResultDestinationError[] = L"destination_error";
constexpr wchar_t kResultCompleted[] = L"completed";
constexpr wchar_t kSecureSubdirectorySddl[] =
    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
constexpr size_t kMaxFailureSamples = 5;
constexpr UINT kDefaultDpi = 96;
constexpr DWORD kResponsivePumpStride = 32;
constexpr ULONGLONG kShareFeedbackDurationMs = 4000;

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

bool GetAppDataDirectory(std::wstring* app_data_directory) {
  std::wstring path;
  if (!GetKnownFolderSubdirectory(FOLDERID_LocalAppData, kAppDataRootSuffix,
                                  &path)) {
    return false;
  }

  return NormalizePath(path, app_data_directory);
}

bool GetShareMetricsPath(std::wstring* metrics_path) {
  std::wstring app_data_directory;
  if (!GetAppDataDirectory(&app_data_directory) ||
      !EnsureDirectoryChainExists(app_data_directory)) {
    return false;
  }

  metrics_path->assign(app_data_directory);
  metrics_path->append(L"\\").append(kShareMetricsFileName);
  return true;
}

bool GetSettingsPath(std::wstring* settings_path) {
  std::wstring app_data_directory;
  if (!GetAppDataDirectory(&app_data_directory) ||
      !EnsureDirectoryChainExists(app_data_directory)) {
    return false;
  }

  settings_path->assign(app_data_directory);
  settings_path->append(L"\\").append(kSettingsFileName);
  return true;
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

bool AppendUtf8LineFile(const std::wstring& path, const std::wstring& line) {
  CHandle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr));
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  std::wstring content(line);
  content.append(L"\r\n");

  const auto bytes_required = WideCharToMultiByte(
      CP_UTF8, 0, content.c_str(), static_cast<int>(content.size()), nullptr, 0,
      nullptr, nullptr);
  if (bytes_required <= 0) {
    return false;
  }

  std::vector<char> buffer(bytes_required);
  if (WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                          static_cast<int>(content.size()), buffer.data(),
                          static_cast<int>(buffer.size()), nullptr,
                          nullptr) != bytes_required) {
    return false;
  }

  DWORD written = 0;
  return WriteFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                   &written, nullptr) != FALSE &&
         written == static_cast<DWORD>(buffer.size()) &&
         FlushFileBuffers(file) != FALSE;
}

std::wstring TrimWhitespace(const std::wstring& value) {
  auto begin = value.begin();
  while (begin != value.end() && iswspace(*begin)) {
    ++begin;
  }

  auto end = value.end();
  while (end != begin && iswspace(*(end - 1))) {
    --end;
  }

  return std::wstring(begin, end);
}

std::wstring ToLowerCopy(const std::wstring& value) {
  std::wstring lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
  return lowered;
}

bool ContainsStringIgnoreCase(const std::wstring& haystack,
                              const std::wstring& needle) {
  if (needle.empty()) {
    return false;
  }

  const auto lowered_haystack = ToLowerCopy(haystack);
  const auto lowered_needle = ToLowerCopy(needle);
  return lowered_haystack.find(lowered_needle) != std::wstring::npos;
}

bool ContainsFilterValue(const std::vector<std::wstring>& filters,
                         const std::wstring& candidate) {
  const auto normalized_candidate = ToLowerCopy(TrimWhitespace(candidate));
  return std::find_if(filters.begin(), filters.end(),
                      [&](const std::wstring& filter) {
                        return ToLowerCopy(filter) == normalized_candidate;
                      }) != filters.end();
}

bool LoadAppSettings(AppSettings* settings) {
  std::wstring settings_path;
  if (settings == nullptr || !GetSettingsPath(&settings_path)) {
    return false;
  }

  settings->deep_scan_enabled =
      GetPrivateProfileIntW(L"Settings", L"DeepScanEnabled", 1,
                            settings_path.c_str()) != 0;
  settings->missing_files_check_on_startup =
      GetPrivateProfileIntW(L"Settings", L"MissingFilesCheckOnStartup", 0,
                            settings_path.c_str()) != 0;

  const auto filter_count = static_cast<int>(
      GetPrivateProfileIntW(L"ExclusionFilters", L"Count", 0,
                            settings_path.c_str()));
  settings->exclusion_filters.clear();
  for (auto index = 0; index < filter_count; ++index) {
    wchar_t key_name[32]{};
    swprintf_s(key_name, L"Filter%d", index);

    wchar_t buffer[512]{};
    GetPrivateProfileStringW(L"ExclusionFilters", key_name, L"", buffer,
                             _countof(buffer), settings_path.c_str());
    const auto filter_value = TrimWhitespace(buffer);
    if (!filter_value.empty() &&
        !ContainsFilterValue(settings->exclusion_filters, filter_value)) {
      settings->exclusion_filters.push_back(filter_value);
    }
  }

  return true;
}

bool SaveAppSettings(const AppSettings& settings) {
  std::wstring settings_path;
  if (!GetSettingsPath(&settings_path)) {
    return false;
  }

  if (!WritePrivateProfileStringW(
          L"Settings", L"DeepScanEnabled",
          settings.deep_scan_enabled ? L"1" : L"0", settings_path.c_str()) ||
      !WritePrivateProfileStringW(
          L"Settings", L"MissingFilesCheckOnStartup",
          settings.missing_files_check_on_startup ? L"1" : L"0",
          settings_path.c_str())) {
    return false;
  }

  if (!WritePrivateProfileSectionW(L"ExclusionFilters", L"\0\0",
                                   settings_path.c_str())) {
    return false;
  }

  wchar_t count_buffer[32]{};
  swprintf_s(count_buffer, L"%u",
             static_cast<unsigned int>(settings.exclusion_filters.size()));
  if (!WritePrivateProfileStringW(L"ExclusionFilters", L"Count", count_buffer,
                                  settings_path.c_str())) {
    return false;
  }

  for (size_t index = 0; index < settings.exclusion_filters.size(); ++index) {
    wchar_t key_name[32]{};
    swprintf_s(key_name, L"Filter%u", static_cast<unsigned int>(index));
    if (!WritePrivateProfileStringW(L"ExclusionFilters", key_name,
                                    settings.exclusion_filters[index].c_str(),
                                    settings_path.c_str())) {
      return false;
    }
  }

  return true;
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

std::wstring BuildIsoTimestampUtc() {
  SYSTEMTIME now{};
  GetSystemTime(&now);

  wchar_t buffer[32]{};
  swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond);
  return std::wstring(buffer);
}

void TrackShareMetric(const wchar_t* event_name, int reclaimable_count,
                      uint64_t reclaimable_size, int selected_count,
                      uint64_t selected_size, bool last_scan_succeeded) {
  std::wstring metrics_path;
  if (!GetShareMetricsPath(&metrics_path)) {
    return;
  }

  CString line;
  line.Format(L"%s\t%s\t%d\t%I64u\t%d\t%I64u\t%d", BuildIsoTimestampUtc().c_str(),
              event_name, reclaimable_count,
              static_cast<unsigned long long>(reclaimable_size), selected_count,
              static_cast<unsigned long long>(selected_size),
              last_scan_succeeded ? 1 : 0);
  AppendUtf8LineFile(metrics_path, std::wstring(line.GetString()));
}

bool CopyTextToClipboard(HWND owner_window, const std::wstring& text) {
  if (!OpenClipboard(owner_window)) {
    return false;
  }

  const auto close_clipboard = [&]() { CloseClipboard(); };
  if (!EmptyClipboard()) {
    close_clipboard();
    return false;
  }

  const auto bytes =
      static_cast<SIZE_T>((text.size() + 1) * sizeof(wchar_t));
  HGLOBAL global_handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (global_handle == nullptr) {
    close_clipboard();
    return false;
  }

  void* global_data = GlobalLock(global_handle);
  if (global_data == nullptr) {
    GlobalFree(global_handle);
    close_clipboard();
    return false;
  }

  memcpy(global_data, text.c_str(), bytes);
  GlobalUnlock(global_handle);

  if (SetClipboardData(CF_UNICODETEXT, global_handle) == nullptr) {
    GlobalFree(global_handle);
    close_clipboard();
    return false;
  }

  close_clipboard();
  return true;
}

constexpr int kSettingsDialogWidth = 584;
constexpr int kSettingsDialogHeight = 488;
constexpr COLORREF kSettingsDialogBackground = RGB(243, 238, 232);
constexpr COLORREF kSettingsDialogPanelBackground = RGB(249, 246, 241);
constexpr COLORREF kSettingsDialogInputBackground = RGB(255, 253, 250);
constexpr COLORREF kSettingsDialogFooterBackground = RGB(247, 243, 238);
constexpr COLORREF kSettingsDialogText = RGB(38, 34, 30);
constexpr COLORREF kSettingsDialogMutedText = RGB(111, 99, 87);
constexpr COLORREF kSettingsDialogAccent = RGB(173, 110, 66);
constexpr COLORREF kSettingsDialogAccentDark = RGB(144, 90, 55);
constexpr COLORREF kSettingsDialogDanger = RGB(145, 67, 58);
constexpr COLORREF kSettingsDialogBorder = RGB(219, 209, 198);
constexpr COLORREF kSettingsDialogDivider = RGB(227, 218, 208);
constexpr COLORREF kSettingsDialogDisabledText = RGB(155, 145, 137);
constexpr COLORREF kSettingsDialogButtonTextOnAccent = RGB(252, 248, 242);

class SettingsDialog : public CWindowImpl<SettingsDialog> {
 public:
  DECLARE_WND_CLASS(L"PatchCleanerSettingsWindow")

  explicit SettingsDialog(const AppSettings& settings)
      : settings_(settings),
        modal_result_(0),
        dpi_(kDefaultDpi),
        deep_scan_enabled_selection_(settings.deep_scan_enabled) {}

  int DoModal(HWND owner_window) {
    dpi_ = GetWindowDpi(owner_window);

    RECT bounds{0, 0, ScaleForDpi(dpi_, kSettingsDialogWidth),
                ScaleForDpi(dpi_, kSettingsDialogHeight)};
    AdjustWindowRectEx(&bounds, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT);

    if (Create(owner_window, bounds, L"PatchCleaner - Settings",
               WS_POPUP | WS_CAPTION | WS_SYSMENU,
               WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT) == nullptr) {
      return IDCANCEL;
    }

    if (owner_window != nullptr) {
      ::EnableWindow(owner_window, FALSE);
    }

    CenterWindow(owner_window);
    ShowWindow(SW_SHOWNORMAL);
    SetForegroundWindow(m_hWnd);

    MSG message{};
    while (modal_result_ == 0 && IsWindow()) {
      const auto message_result = GetMessage(&message, nullptr, 0, 0);
      if (message_result <= 0) {
        modal_result_ = IDCANCEL;
        break;
      }

      if (!IsDialogMessage(&message)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
      }
    }

    if (IsWindow()) {
      DestroyWindow();
    }

    if (owner_window != nullptr && ::IsWindow(owner_window)) {
      ::EnableWindow(owner_window, TRUE);
      ::SetActiveWindow(owner_window);
      ::SetForegroundWindow(owner_window);
    }

    return modal_result_ == 0 ? IDCANCEL : modal_result_;
  }

  const AppSettings& settings() const {
    return settings_;
  }

  BEGIN_MSG_MAP(SettingsDialog)
    MSG_WM_CREATE(OnCreate)
    MSG_WM_CLOSE(OnClose)
    MSG_WM_DESTROY(OnDestroy)
    MSG_WM_PAINT(OnPaint)
    MSG_WM_ERASEBKGND(OnEraseBkgnd)
    COMMAND_ID_HANDLER_EX(kControlDeepScanOn, OnDeepScanToggle)
    COMMAND_ID_HANDLER_EX(kControlDeepScanOff, OnDeepScanToggle)
    COMMAND_ID_HANDLER_EX(kControlAddFilter, OnAddFilter)
    COMMAND_ID_HANDLER_EX(kControlRemoveFilter, OnRemoveFilter)
    COMMAND_ID_HANDLER_EX(IDOK, OnSave)
    COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
    COMMAND_HANDLER_EX(kControlFilterList, LBN_SELCHANGE,
                       OnFilterSelectionChange)
    MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
    MESSAGE_HANDLER(WM_CTLCOLORDLG, OnCtlColorDialog)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
    MESSAGE_HANDLER(WM_CTLCOLORBTN, OnCtlColorStatic)
    MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnCtlColorEdit)
    MESSAGE_HANDLER(WM_CTLCOLORLISTBOX, OnCtlColorEdit)
  END_MSG_MAP()

 private:
  enum ControlIds {
    kControlDeepScanOn = 5001,
    kControlDeepScanOff = 5002,
    kControlFilterText = 5003,
    kControlAddFilter = 5004,
    kControlRemoveFilter = 5005,
    kControlFilterList = 5006,
    kControlStartupCheck = 5007,
  };

  int OnCreate(CREATESTRUCT* /*create*/) {
    InitializeFonts();

    background_brush_.CreateSolidBrush(kSettingsDialogBackground);
    panel_brush_.CreateSolidBrush(kSettingsDialogPanelBackground);
    footer_brush_.CreateSolidBrush(kSettingsDialogFooterBackground);
    input_brush_.CreateSolidBrush(kSettingsDialogInputBackground);

    const auto scale = [&](int value) { return ScaleForDpi(dpi_, value); };
    const auto apply_font = [&](HWND window, HFONT font) {
      if (window != nullptr) {
        SendMessage(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      }
    };
    const auto create_static = [&](const wchar_t* text, DWORD style, int x,
                                   int y, int width, int height,
                                   HFONT font) -> HWND {
      HWND window = ::CreateWindowExW(
          0, L"STATIC", text,
          WS_CHILD | WS_VISIBLE | style | SS_LEFT,
          scale(x), scale(y), scale(width), scale(height), m_hWnd, nullptr,
          _AtlBaseModule.GetModuleInstance(), nullptr);
      apply_font(window, font);
      return window;
    };
    const auto create_button = [&](const wchar_t* text, DWORD style,
                                   DWORD ex_style, int x, int y, int width,
                                   int height, int control_id,
                                   HFONT font) -> HWND {
      HWND window = ::CreateWindowExW(
          ex_style, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
          scale(x), scale(y), scale(width), scale(height), m_hWnd,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
          _AtlBaseModule.GetModuleInstance(), nullptr);
      apply_font(window, font);
      return window;
    };

    hero_eyebrow_.Attach(create_static(L"UTILITY CONTROLS", 0, 28, 22, 180, 16,
                                       heading_font_));
    hero_title_.Attach(
        create_static(L"Settings", 0, 28, 40, 220, 30, title_font_));
    hero_body_.Attach(create_static(
        L"Tune scan depth, exclusion rules, and startup behavior without "
        L"leaving the Patch Cleaner workflow.",
        SS_LEFT, 28, 74, 286, 34, body_font_));

    deep_scan_heading_.Attach(
        create_static(L"Deep Scan", 0, 352, 30, 120, 18, heading_font_));
    deep_scan_caption_.Attach(create_static(
        L"Metadata + signer matching", 0, 352, 50, 172, 16, caption_font_));
    create_button(L"On", BS_AUTORADIOBUTTON | BS_PUSHLIKE | BS_OWNERDRAW |
                            WS_TABSTOP | WS_GROUP,
                  0, 352, 72, 80, 34, kControlDeepScanOn, button_font_);
    create_button(L"Off", BS_AUTORADIOBUTTON | BS_PUSHLIKE | BS_OWNERDRAW, 0,
                  438, 72, 80, 34, kControlDeepScanOff, button_font_);

    deep_scan_body_.Attach(create_static(
        L"The deep scan reads MSI/MSP metadata and signer details to improve "
        L"filtering. It uses more memory and can slow large scans down, but "
        L"it helps avoid false positives.",
        SS_LEFT, 28, 114, 500, 34, body_font_));

    filter_heading_.Attach(
        create_static(L"Exclusion Filter", 0, 28, 178, 200, 18, heading_font_));
    filter_body_.Attach(create_static(
        L"Add contains-filters to hide known-safe orphaned files. Patch "
        L"Cleaner matches file paths immediately and, with Deep Scan on, also "
        L"checks MSI/MSP title, subject, author, and signer information.",
        SS_LEFT, 28, 204, 512, 54, body_font_));

    CRect filter_edit_rect(scale(28), scale(266), scale(440), scale(298));
    filter_edit_.Create(m_hWnd, filter_edit_rect, nullptr,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        WS_EX_CLIENTEDGE, kControlFilterText);
    filter_edit_.SetFont(body_font_);

    add_filter_button_.Attach(create_button(
        L"+", BS_OWNERDRAW | WS_TABSTOP, 0, 448, 265, 32, 32,
        kControlAddFilter, button_font_));
    remove_filter_button_.Attach(create_button(
        L"-", BS_OWNERDRAW | WS_TABSTOP, 0, 486, 265, 32, 32,
        kControlRemoveFilter, button_font_));

    CRect filter_list_rect(scale(28), scale(312), scale(540), scale(400));
    filter_list_.Create(m_hWnd, filter_list_rect, nullptr,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                        WS_EX_CLIENTEDGE, kControlFilterList);
    filter_list_.SetFont(body_font_);

    startup_check_.Attach(create_button(
        L"Perform Missing Files Check on startup",
        BS_AUTOCHECKBOX | WS_TABSTOP, 0, 28, 426, 300, 24,
        kControlStartupCheck, body_font_));
    footer_hint_.Attach(create_static(
        L"Applies to the next scan and is stored locally on this PC.",
        SS_LEFT, 28, 452, 320, 16, caption_font_));

    save_button_.Attach(create_button(L"Save", BS_OWNERDRAW | WS_TABSTOP, 0,
                                      388, 424, 72, 32, IDOK, button_font_));
    cancel_button_.Attach(create_button(L"Cancel", BS_OWNERDRAW | WS_TABSTOP,
                                        0, 468, 424, 84, 32, IDCANCEL,
                                        button_font_));

    deep_scan_on_.Attach(GetDlgItem(kControlDeepScanOn));
    deep_scan_off_.Attach(GetDlgItem(kControlDeepScanOff));
    UpdateDeepScanButtons();
    startup_check_.SetCheck(settings_.missing_files_check_on_startup
                                ? BST_CHECKED
                                : BST_UNCHECKED);

    RefreshFilterList();
    filter_edit_.SetFocus();
    return 0;
  }

  void OnClose() {
    EndModal(IDCANCEL);
  }

  void OnDestroy() {
    if (!heading_font_.IsNull()) {
      heading_font_.DeleteObject();
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
    if (!background_brush_.IsNull()) {
      background_brush_.DeleteObject();
    }
    if (!panel_brush_.IsNull()) {
      panel_brush_.DeleteObject();
    }
    if (!footer_brush_.IsNull()) {
      footer_brush_.DeleteObject();
    }
    if (!input_brush_.IsNull()) {
      input_brush_.DeleteObject();
    }
  }

  void OnPaint(CDCHandle dc) {
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

    FillRectColor(target_dc, client_rect, kSettingsDialogBackground);

    const auto scale = [&](int value) { return ScaleForDpi(dpi_, value); };

    CRect top_panel(scale(16), scale(14), client_rect.right - scale(16),
                    scale(154));
    FillRoundedRect(target_dc, top_panel, kSettingsDialogPanelBackground,
                    BlendColor(kSettingsDialogBorder, kSettingsDialogAccent, 14),
                    scale(18));
    CRect top_rule = top_panel;
    top_rule.left += scale(18);
    top_rule.right -= scale(18);
    top_rule.top += scale(12);
    top_rule.bottom = top_rule.top + scale(3);
    FillRectColor(target_dc, top_rule, kSettingsDialogAccent);

    CRect filters_panel(scale(16), scale(170), client_rect.right - scale(16),
                        scale(410));
    FillRoundedRect(target_dc, filters_panel, kSettingsDialogPanelBackground,
                    kSettingsDialogBorder, scale(18));
    CRect filters_rule = filters_panel;
    filters_rule.left += scale(18);
    filters_rule.right -= scale(18);
    filters_rule.top += scale(14);
    filters_rule.bottom = filters_rule.top + 1;
    FillRectColor(target_dc, filters_rule,
                  BlendColor(kSettingsDialogDivider, kSettingsDialogAccent, 24));

    CRect footer_panel(scale(16), scale(416), client_rect.right - scale(16),
                       client_rect.bottom - scale(16));
    FillRoundedRect(target_dc, footer_panel, kSettingsDialogFooterBackground,
                    kSettingsDialogBorder, scale(18));
    CRect footer_rule = footer_panel;
    footer_rule.left += scale(18);
    footer_rule.right -= scale(18);
    footer_rule.top += scale(12);
    footer_rule.bottom = footer_rule.top + 1;
    FillRectColor(target_dc, footer_rule, kSettingsDialogDivider);
  }

  BOOL OnEraseBkgnd(CDCHandle /*dc*/) {
    return TRUE;
  }

  LRESULT OnDrawItem(UINT /*message*/, WPARAM /*w_param*/, LPARAM l_param,
                     BOOL& handled) {
    handled = TRUE;

    const auto* draw =
        reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
    if (draw == nullptr || draw->CtlType != ODT_BUTTON) {
      handled = FALSE;
      return 0;
    }

    const auto scale = [&](int value) { return ScaleForDpi(dpi_, value); };
    const auto control_id = static_cast<int>(draw->CtlID);
    const auto enabled = (draw->itemState & ODS_DISABLED) == 0;
    const auto hot = (draw->itemState & ODS_HOTLIGHT) != 0;
    const auto pressed = (draw->itemState & ODS_SELECTED) != 0;
    const auto segmented =
        control_id == kControlDeepScanOn || control_id == kControlDeepScanOff;
    const auto checked =
        segmented &&
        ((control_id == kControlDeepScanOn && deep_scan_enabled_selection_) ||
         (control_id == kControlDeepScanOff && !deep_scan_enabled_selection_));
    const auto primary = control_id == IDOK || control_id == kControlAddFilter;
    const auto danger = control_id == kControlRemoveFilter;

    COLORREF fill_color =
        primary ? kSettingsDialogAccent : kSettingsDialogFooterBackground;
    COLORREF border_color =
        primary ? kSettingsDialogAccentDark : kSettingsDialogBorder;
    COLORREF text_color =
        primary ? kSettingsDialogButtonTextOnAccent : kSettingsDialogText;
    if (segmented) {
      fill_color = checked ? BlendColor(kSettingsDialogAccentDark,
                                        kSettingsDialogAccent, 72)
                           : BlendColor(kSettingsDialogInputBackground,
                                        kSettingsDialogBackground, 22);
      border_color =
          checked ? kSettingsDialogAccentDark
                  : BlendColor(kSettingsDialogBorder, kSettingsDialogAccent, 18);
      text_color = checked ? kSettingsDialogButtonTextOnAccent
                           : kSettingsDialogMutedText;
      if (hot && !checked) {
        fill_color =
            BlendColor(kSettingsDialogInputBackground, kSettingsDialogAccent, 12);
      }
    } else if (!enabled) {
      fill_color = primary ? BlendColor(kSettingsDialogBackground,
                                        kSettingsDialogAccent, 12)
                           : RGB(233, 227, 220);
      border_color = BlendColor(kSettingsDialogBorder,
                                kSettingsDialogFooterBackground, 20);
      text_color = kSettingsDialogDisabledText;
    } else if (danger) {
      fill_color = hot ? BlendColor(kSettingsDialogFooterBackground,
                                    kSettingsDialogDanger, 16)
                       : BlendColor(kSettingsDialogFooterBackground,
                                    kSettingsDialogDanger, 6);
      border_color = BlendColor(kSettingsDialogDanger, kSettingsDialogFooterBackground,
                                hot ? 5 : 18);
      text_color =
          hot ? kSettingsDialogDanger
              : BlendColor(kSettingsDialogText, kSettingsDialogDanger, 70);
    } else if (!primary) {
      fill_color = hot ? BlendColor(kSettingsDialogFooterBackground,
                                    kSettingsDialogAccent, 10)
                       : kSettingsDialogFooterBackground;
      border_color = hot ? BlendColor(kSettingsDialogBorder,
                                      kSettingsDialogAccent, 45)
                         : kSettingsDialogBorder;
    }

    if (pressed && enabled) {
      fill_color = BlendColor(fill_color, RGB(32, 28, 24), 10);
      border_color = BlendColor(border_color, RGB(32, 28, 24), 12);
    }

    CDCHandle control_dc(draw->hDC);
    CRect rect(draw->rcItem);
    FillRoundedRect(control_dc, rect, fill_color, border_color,
                    segmented ? scale(16) : scale(12));
    if (segmented && checked) {
      CRect inner_rect(rect);
      inner_rect.DeflateRect(scale(3), scale(3));
      DrawRectOutline(control_dc, inner_rect,
                      BlendColor(kSettingsDialogButtonTextOnAccent,
                                 kSettingsDialogAccentDark, 24));
    }

    wchar_t buffer[128]{};
    ::GetWindowTextW(draw->hwndItem, buffer, _countof(buffer));
    const auto old_font = control_dc.SelectFont(button_font_);
    const auto old_mode = control_dc.SetBkMode(TRANSPARENT);
    const auto old_color = control_dc.SetTextColor(text_color);
    CRect text_rect(rect);
    control_dc.DrawText(buffer, -1, &text_rect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    control_dc.SetTextColor(old_color);
    control_dc.SetBkMode(old_mode);
    control_dc.SelectFont(old_font);

    if ((draw->itemState & ODS_FOCUS) != 0) {
      CRect focus_rect(rect);
      focus_rect.DeflateRect(scale(4), scale(4));
      control_dc.DrawFocusRect(&focus_rect);
    }

    return TRUE;
  }

  void OnAddFilter(UINT /*notify_code*/, int /*id*/, CWindow /*control*/) {
    AddFilterFromEdit();
  }

  void OnDeepScanToggle(UINT /*notify_code*/, int id, CWindow /*control*/) {
    deep_scan_enabled_selection_ = id == kControlDeepScanOn;
    UpdateDeepScanButtons();
  }

  void OnRemoveFilter(UINT /*notify_code*/, int /*id*/, CWindow /*control*/) {
    const auto selected_index = filter_list_.GetCurSel();
    if (selected_index < 0 ||
        selected_index >= static_cast<int>(settings_.exclusion_filters.size())) {
      return;
    }

    settings_.exclusion_filters.erase(settings_.exclusion_filters.begin() +
                                      selected_index);
    RefreshFilterList();
    if (selected_index < filter_list_.GetCount()) {
      filter_list_.SetCurSel(selected_index);
    } else if (filter_list_.GetCount() > 0) {
      filter_list_.SetCurSel(filter_list_.GetCount() - 1);
    }
    UpdateRemoveButton();
  }

  void OnSave(UINT /*notify_code*/, int /*id*/, CWindow /*control*/) {
    AddFilterFromEdit();
    settings_.deep_scan_enabled = deep_scan_enabled_selection_;
    settings_.missing_files_check_on_startup =
        startup_check_.GetCheck() == BST_CHECKED;
    EndModal(IDOK);
  }

  void OnCancel(UINT /*notify_code*/, int /*id*/, CWindow /*control*/) {
    EndModal(IDCANCEL);
  }

  void OnFilterSelectionChange(UINT /*notify_code*/, int /*id*/,
                               CWindow /*control*/) {
    UpdateRemoveButton();
  }

  LRESULT OnCtlColorDialog(UINT /*message*/, WPARAM w_param, LPARAM /*l_param*/,
                           BOOL& handled) {
    handled = TRUE;
    SetBkColor(reinterpret_cast<HDC>(w_param), kSettingsDialogBackground);
    return reinterpret_cast<LRESULT>(background_brush_.m_hBrush);
  }

  LRESULT OnCtlColorStatic(UINT /*message*/, WPARAM w_param, LPARAM l_param,
                           BOOL& handled) {
    handled = TRUE;
    const auto child_window = reinterpret_cast<HWND>(l_param);
    auto text_color = kSettingsDialogText;
    if (child_window == hero_eyebrow_.m_hWnd) {
      text_color = kSettingsDialogAccent;
    } else if (child_window == hero_title_.m_hWnd) {
      text_color = kSettingsDialogText;
    } else if (child_window == hero_body_.m_hWnd ||
               child_window == deep_scan_body_.m_hWnd ||
               child_window == filter_body_.m_hWnd ||
               child_window == deep_scan_caption_.m_hWnd ||
               child_window == footer_hint_.m_hWnd) {
      text_color = kSettingsDialogMutedText;
    } else if (child_window == deep_scan_heading_.m_hWnd ||
               child_window == filter_heading_.m_hWnd) {
      text_color = kSettingsDialogAccentDark;
    }

    SetTextColor(reinterpret_cast<HDC>(w_param), text_color);
    SetBkMode(reinterpret_cast<HDC>(w_param), TRANSPARENT);
    return reinterpret_cast<LRESULT>(GetSectionBrush(child_window));
  }

  LRESULT OnCtlColorEdit(UINT /*message*/, WPARAM w_param, LPARAM /*l_param*/,
                         BOOL& handled) {
    handled = TRUE;
    SetTextColor(reinterpret_cast<HDC>(w_param), kSettingsDialogText);
    SetBkColor(reinterpret_cast<HDC>(w_param), kSettingsDialogInputBackground);
    return reinterpret_cast<LRESULT>(input_brush_.m_hBrush);
  }

  void InitializeFonts() {
    const auto base_font = BuildBaseUiFont();
    const auto create_font = [&](CFont* font, int point_size, LONG weight) {
      LOGFONT face = base_font;
      face.lfHeight = -MulDiv(point_size, static_cast<int>(dpi_), 72);
      face.lfWeight = weight;
      face.lfQuality = CLEARTYPE_NATURAL_QUALITY;
      font->CreateFontIndirect(&face);
    };

    create_font(&heading_font_, 9, FW_SEMIBOLD);
    create_font(&title_font_, 21, FW_SEMIBOLD);
    create_font(&body_font_, 10, FW_NORMAL);
    create_font(&caption_font_, 9, FW_NORMAL);
    create_font(&button_font_, 10, FW_SEMIBOLD);
  }

  bool AddFilterFromEdit() {
    const auto text_length = filter_edit_.GetWindowTextLength();
    if (text_length <= 0) {
      return false;
    }

    CString filter_text;
    filter_edit_.GetWindowText(filter_text);
    const auto normalized_filter =
        TrimWhitespace(std::wstring(filter_text.GetString()));
    if (normalized_filter.empty()) {
      filter_edit_.SetWindowText(L"");
      return false;
    }

    if (!ContainsFilterValue(settings_.exclusion_filters, normalized_filter)) {
      settings_.exclusion_filters.push_back(normalized_filter);
      RefreshFilterList();
      filter_list_.SetCurSel(filter_list_.GetCount() - 1);
    }

    filter_edit_.SetWindowText(L"");
    UpdateRemoveButton();
    return true;
  }

  void RefreshFilterList() {
    filter_list_.ResetContent();
    for (const auto& filter : settings_.exclusion_filters) {
      filter_list_.AddString(filter.c_str());
    }
    UpdateRemoveButton();
  }

  void UpdateDeepScanButtons() {
    if (deep_scan_on_.IsWindow()) {
      deep_scan_on_.SetCheck(deep_scan_enabled_selection_ ? BST_CHECKED
                                                          : BST_UNCHECKED);
      deep_scan_on_.Invalidate();
    }
    if (deep_scan_off_.IsWindow()) {
      deep_scan_off_.SetCheck(deep_scan_enabled_selection_ ? BST_UNCHECKED
                                                           : BST_CHECKED);
      deep_scan_off_.Invalidate();
    }
  }

  void UpdateRemoveButton() {
    remove_filter_button_.EnableWindow(filter_list_.GetCurSel() >= 0);
  }

  void EndModal(int result) {
    modal_result_ = result;
    if (IsWindow()) {
      DestroyWindow();
    }
  }

  HBRUSH GetSectionBrush(HWND child_window) const {
    if (child_window == nullptr) {
      return background_brush_.m_hBrush;
    }

    RECT child_rect{};
    if (!::GetWindowRect(child_window, &child_rect) ||
        !::MapWindowPoints(HWND_DESKTOP, m_hWnd,
                           reinterpret_cast<POINT*>(&child_rect), 2)) {
      return background_brush_.m_hBrush;
    }

    return child_rect.top >= ScaleForDpi(dpi_, 416) ? footer_brush_.m_hBrush
                                                    : panel_brush_.m_hBrush;
  }

  AppSettings settings_;
  int modal_result_;
  UINT dpi_;
  bool deep_scan_enabled_selection_;
  CBrush background_brush_;
  CBrush panel_brush_;
  CBrush footer_brush_;
  CBrush input_brush_;
  CFont heading_font_;
  CFont title_font_;
  CFont body_font_;
  CFont caption_font_;
  CFont button_font_;
  CWindow hero_eyebrow_;
  CWindow hero_title_;
  CWindow hero_body_;
  CWindow deep_scan_heading_;
  CWindow deep_scan_caption_;
  CWindow deep_scan_body_;
  CWindow filter_heading_;
  CWindow filter_body_;
  CWindow footer_hint_;
  CButton deep_scan_on_;
  CButton deep_scan_off_;
  CEdit filter_edit_;
  CButton add_filter_button_;
  CButton remove_filter_button_;
  CListBox filter_list_;
  CButton startup_check_;
  CButton save_button_;
  CButton cancel_button_;
};

struct CertContextDeleter {
  void operator()(const CERT_CONTEXT* certificate) const {
    if (certificate != nullptr) {
      CertFreeCertificateContext(certificate);
    }
  }
};

bool ReadSummaryPropertyString(const std::wstring& path, UINT property_id,
                               std::wstring* value) {
  if (value == nullptr) {
    return false;
  }

  MSIHANDLE summary_handle = 0;
  if (MsiGetSummaryInformationW(0, path.c_str(), 0, &summary_handle) !=
      ERROR_SUCCESS) {
    return false;
  }

  const auto close_handle = [&]() {
    if (summary_handle != 0) {
      MsiCloseHandle(summary_handle);
      summary_handle = 0;
    }
  };

  UINT data_type = 0;
  INT integer_value = 0;
  FILETIME time_value{};
  DWORD length = 0;
  auto result =
      MsiSummaryInfoGetPropertyW(summary_handle, property_id, &data_type,
                                 &integer_value, &time_value, nullptr, &length);
  if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) {
    close_handle();
    return false;
  }

  if (length == 0) {
    value->clear();
    close_handle();
    return result == ERROR_SUCCESS;
  }

  std::vector<wchar_t> buffer(length + 1, L'\0');
  result = MsiSummaryInfoGetPropertyW(summary_handle, property_id, &data_type,
                                      &integer_value, &time_value,
                                      buffer.data(), &length);
  close_handle();
  if (result != ERROR_SUCCESS) {
    return false;
  }

  value->assign(buffer.data(), length);
  return true;
}

void AppendMetadataValue(const std::wstring& value, std::wstring* metadata) {
  if (metadata == nullptr || value.empty()) {
    return;
  }

  if (!metadata->empty()) {
    metadata->append(L"\n");
  }
  metadata->append(value);
}

std::wstring GetSignerDisplayName(const std::wstring& path) {
  PCCERT_CONTEXT raw_certificate = nullptr;
  if (FAILED(MsiGetFileSignatureInformationW(path.c_str(), 0, &raw_certificate,
                                             nullptr, nullptr)) ||
      raw_certificate == nullptr) {
    return std::wstring();
  }

  std::unique_ptr<const CERT_CONTEXT, CertContextDeleter> certificate(
      raw_certificate);
  const auto read_name = [&](DWORD flags) {
    const auto size = CertGetNameStringW(
        certificate.get(), CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr,
        nullptr, 0);
    if (size <= 1) {
      return std::wstring();
    }

    std::vector<wchar_t> buffer(size);
    if (CertGetNameStringW(certificate.get(), CERT_NAME_SIMPLE_DISPLAY_TYPE,
                           flags, nullptr, buffer.data(),
                           static_cast<DWORD>(buffer.size())) <= 1) {
      return std::wstring();
    }

    return std::wstring(buffer.data());
  };

  const auto subject_name = read_name(0);
  const auto issuer_name = read_name(CERT_NAME_ISSUER_FLAG);
  if (subject_name.empty()) {
    return issuer_name;
  }
  if (issuer_name.empty() || _wcsicmp(subject_name.c_str(), issuer_name.c_str()) == 0) {
    return subject_name;
  }

  return subject_name + L" " + issuer_name;
}

std::wstring BuildDeepScanMetadata(const std::wstring& path) {
  std::wstring metadata;

  std::wstring title;
  if (ReadSummaryPropertyString(path, PID_TITLE, &title)) {
    AppendMetadataValue(title, &metadata);
  }

  std::wstring subject;
  if (ReadSummaryPropertyString(path, PID_SUBJECT, &subject)) {
    AppendMetadataValue(subject, &metadata);
  }

  std::wstring author;
  if (ReadSummaryPropertyString(path, PID_AUTHOR, &author)) {
    AppendMetadataValue(author, &metadata);
  }

  AppendMetadataValue(GetSignerDisplayName(path), &metadata);
  return metadata;
}

std::wstring ExtractFileName(const std::wstring& path) {
  const auto separator_index = path.find_last_of(L"\\/");
  if (separator_index == std::wstring::npos) {
    return path;
  }

  return path.substr(separator_index + 1);
}

std::wstring ValueOrFallback(const std::wstring& value,
                             const wchar_t* fallback) {
  return value.empty() ? std::wstring(fallback) : value;
}

struct FileDetailsModel {
  std::wstring file_name;
  std::wstring file_path;
  std::wstring title;
  std::wstring subject;
  std::wstring author;
  std::wstring digital_signature;
  std::wstring file_size;
  std::wstring comment;
};

FileDetailsModel BuildFileDetailsModel(const FileItem& item) {
  FileDetailsModel model;
  model.file_name = ExtractFileName(item.path);
  model.file_path = item.path;

  std::wstring title;
  ReadSummaryPropertyString(item.path, PID_TITLE, &title);
  model.title = ValueOrFallback(title, L"Not available");

  std::wstring subject;
  ReadSummaryPropertyString(item.path, PID_SUBJECT, &subject);
  model.subject = ValueOrFallback(subject, L"Not available");

  std::wstring author;
  ReadSummaryPropertyString(item.path, PID_AUTHOR, &author);
  model.author = ValueOrFallback(author, L"Not available");

  std::wstring comment;
  ReadSummaryPropertyString(item.path, PID_COMMENTS, &comment);
  model.comment = ValueOrFallback(comment, L"No summary comment provided.");

  model.digital_signature =
      ValueOrFallback(GetSignerDisplayName(item.path),
                      L"Not signed or certificate details unavailable.");
  model.file_size = std::wstring(FormatSizeString(item.size).GetString());
  if (model.file_name.empty()) {
    model.file_name = model.file_path;
  }
  return model;
}

constexpr int kFileDetailsDialogWidth = 696;
constexpr int kFileDetailsDialogHeight = 496;

class FileDetailsDialog : public CWindowImpl<FileDetailsDialog> {
 public:
  DECLARE_WND_CLASS(L"PatchCleanerFileDetailsWindow")

  explicit FileDetailsDialog(const FileDetailsModel& details)
      : details_(details), modal_result_(0), dpi_(kDefaultDpi) {}

  int DoModal(HWND owner_window) {
    dpi_ = GetWindowDpi(owner_window);

    RECT bounds{0, 0, ScaleForDpi(dpi_, kFileDetailsDialogWidth),
                ScaleForDpi(dpi_, kFileDetailsDialogHeight)};
    AdjustWindowRectEx(&bounds, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT);

    if (Create(owner_window, bounds, L"PatchCleaner - File Details",
               WS_POPUP | WS_CAPTION | WS_SYSMENU,
               WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT) == nullptr) {
      return IDCANCEL;
    }

    if (owner_window != nullptr) {
      ::EnableWindow(owner_window, FALSE);
    }

    CenterWindow(owner_window);
    ShowWindow(SW_SHOWNORMAL);
    SetForegroundWindow(m_hWnd);

    MSG message{};
    while (modal_result_ == 0 && IsWindow()) {
      const auto message_result = GetMessage(&message, nullptr, 0, 0);
      if (message_result <= 0) {
        modal_result_ = IDCANCEL;
        break;
      }

      if (!IsDialogMessage(&message)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
      }
    }

    if (IsWindow()) {
      DestroyWindow();
    }

    if (owner_window != nullptr && ::IsWindow(owner_window)) {
      ::EnableWindow(owner_window, TRUE);
      ::SetActiveWindow(owner_window);
      ::SetForegroundWindow(owner_window);
    }

    return modal_result_ == 0 ? IDCANCEL : modal_result_;
  }

  BEGIN_MSG_MAP(FileDetailsDialog)
    MSG_WM_CREATE(OnCreate)
    MSG_WM_CLOSE(OnClose)
    MSG_WM_DESTROY(OnDestroy)
    MSG_WM_PAINT(OnPaint)
    MSG_WM_ERASEBKGND(OnEraseBkgnd)
    COMMAND_ID_HANDLER_EX(IDCANCEL, OnCloseCommand)
    MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
    MESSAGE_HANDLER(WM_CTLCOLORDLG, OnCtlColorDialog)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
    MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnCtlColorEdit)
  END_MSG_MAP()

 private:
  int OnCreate(CREATESTRUCT* /*create*/) {
    InitializeFonts();

    background_brush_.CreateSolidBrush(kSettingsDialogBackground);
    panel_brush_.CreateSolidBrush(kSettingsDialogPanelBackground);
    footer_brush_.CreateSolidBrush(kSettingsDialogFooterBackground);
    input_brush_.CreateSolidBrush(kSettingsDialogInputBackground);

    const auto scale = [&](int value) { return ScaleForDpi(dpi_, value); };
    const auto apply_font = [&](HWND window, HFONT font) {
      if (window != nullptr) {
        SendMessage(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      }
    };
    const auto create_static = [&](const wchar_t* text, DWORD style, int x,
                                   int y, int width, int height,
                                   HFONT font) -> HWND {
      HWND window = ::CreateWindowExW(
          0, L"STATIC", text,
          WS_CHILD | WS_VISIBLE | style | SS_LEFT,
          scale(x), scale(y), scale(width), scale(height), m_hWnd, nullptr,
          _AtlBaseModule.GetModuleInstance(), nullptr);
      apply_font(window, font);
      return window;
    };
    const auto create_edit = [&](const std::wstring& text, DWORD style, int x,
                                 int y, int width, int height,
                                 HFONT font) -> HWND {
      HWND window = ::CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
          WS_CHILD | WS_VISIBLE | style,
          scale(x), scale(y), scale(width), scale(height), m_hWnd, nullptr,
          _AtlBaseModule.GetModuleInstance(), nullptr);
      apply_font(window, font);
      return window;
    };
    const auto create_button = [&](const wchar_t* text, DWORD style, int x,
                                   int y, int width, int height, int control_id,
                                   HFONT font) -> HWND {
      HWND window = ::CreateWindowExW(
          0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
          scale(x), scale(y), scale(width), scale(height), m_hWnd,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
          _AtlBaseModule.GetModuleInstance(), nullptr);
      apply_font(window, font);
      return window;
    };

    eyebrow_.Attach(create_static(L"ORPHANED FILE DETAILS", 0, 28, 24, 220, 16,
                                  heading_font_));
    title_.Attach(create_static(details_.file_name.c_str(), 0, 28, 44, 620, 30,
                                title_font_));
    path_value_.Attach(create_edit(details_.file_path,
                                   ES_AUTOHSCROLL | ES_READONLY | ES_NOHIDESEL,
                                   28, 78, 636, 24, body_font_));

    title_label_.Attach(
        create_static(L"Title", SS_RIGHT, 28, 150, 100, 18, label_font_));
    title_value_.Attach(create_edit(details_.title,
                                    ES_AUTOHSCROLL | ES_READONLY |
                                        ES_NOHIDESEL,
                                    142, 144, 522, 28, body_font_));

    subject_label_.Attach(
        create_static(L"Subject", SS_RIGHT, 28, 188, 100, 18, label_font_));
    subject_value_.Attach(create_edit(details_.subject,
                                      ES_MULTILINE | ES_AUTOVSCROLL |
                                          ES_READONLY | ES_NOHIDESEL |
                                          WS_VSCROLL,
                                      142, 182, 522, 44, body_font_));

    author_label_.Attach(
        create_static(L"Author", SS_RIGHT, 28, 242, 100, 18, label_font_));
    author_value_.Attach(create_edit(details_.author,
                                     ES_AUTOHSCROLL | ES_READONLY |
                                         ES_NOHIDESEL,
                                     142, 236, 522, 28, body_font_));

    signature_label_.Attach(create_static(L"Digital Signature", SS_RIGHT, 12,
                                          282, 116, 18, label_font_));
    signature_value_.Attach(create_edit(
        details_.digital_signature,
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL |
            WS_VSCROLL,
        142, 276, 522, 76, body_font_));

    size_label_.Attach(
        create_static(L"File Size", SS_RIGHT, 28, 368, 100, 18, label_font_));
    size_value_.Attach(create_edit(details_.file_size,
                                   ES_AUTOHSCROLL | ES_READONLY | ES_NOHIDESEL,
                                   142, 362, 210, 28, body_font_));

    comment_label_.Attach(
        create_static(L"Comment", SS_RIGHT, 28, 404, 100, 18, label_font_));
    comment_value_.Attach(create_edit(details_.comment,
                                      ES_MULTILINE | ES_AUTOVSCROLL |
                                          ES_READONLY | ES_NOHIDESEL |
                                          WS_VSCROLL,
                                      142, 398, 522, 42, body_font_));

    close_button_.Attach(create_button(L"Close", BS_OWNERDRAW | WS_TABSTOP,
                                       576, 444, 88, 32, IDCANCEL,
                                       button_font_));
    return 0;
  }

  void OnClose() {
    EndModal(IDCANCEL);
  }

  void OnDestroy() {
    if (!heading_font_.IsNull()) {
      heading_font_.DeleteObject();
    }
    if (!title_font_.IsNull()) {
      title_font_.DeleteObject();
    }
    if (!label_font_.IsNull()) {
      label_font_.DeleteObject();
    }
    if (!body_font_.IsNull()) {
      body_font_.DeleteObject();
    }
    if (!button_font_.IsNull()) {
      button_font_.DeleteObject();
    }
    if (!background_brush_.IsNull()) {
      background_brush_.DeleteObject();
    }
    if (!panel_brush_.IsNull()) {
      panel_brush_.DeleteObject();
    }
    if (!footer_brush_.IsNull()) {
      footer_brush_.DeleteObject();
    }
    if (!input_brush_.IsNull()) {
      input_brush_.DeleteObject();
    }
  }

  void OnPaint(CDCHandle dc) {
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

    const auto scale = [&](int value) { return ScaleForDpi(dpi_, value); };
    FillRectColor(target_dc, client_rect, kSettingsDialogBackground);

    CRect top_panel(scale(16), scale(14), client_rect.right - scale(16),
                    scale(114));
    FillRoundedRect(target_dc, top_panel, kSettingsDialogPanelBackground,
                    BlendColor(kSettingsDialogBorder, kSettingsDialogAccent, 14),
                    scale(18));

    CRect top_rule = top_panel;
    top_rule.left += scale(18);
    top_rule.right -= scale(18);
    top_rule.top += scale(12);
    top_rule.bottom = top_rule.top + scale(3);
    FillRectColor(target_dc, top_rule, kSettingsDialogAccent);

    CRect content_panel(scale(16), scale(126), client_rect.right - scale(16),
                        scale(436));
    FillRoundedRect(target_dc, content_panel, kSettingsDialogPanelBackground,
                    kSettingsDialogBorder, scale(18));

    CRect footer_panel(scale(16), scale(438), client_rect.right - scale(16),
                       client_rect.bottom - scale(16));
    FillRoundedRect(target_dc, footer_panel, kSettingsDialogFooterBackground,
                    kSettingsDialogBorder, scale(18));
  }

  BOOL OnEraseBkgnd(CDCHandle /*dc*/) {
    return TRUE;
  }

  void OnCloseCommand(UINT /*notify_code*/, int /*id*/, CWindow /*control*/) {
    EndModal(IDCANCEL);
  }

  LRESULT OnDrawItem(UINT /*message*/, WPARAM /*w_param*/, LPARAM l_param,
                     BOOL& handled) {
    handled = TRUE;

    const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
    if (draw == nullptr || draw->CtlType != ODT_BUTTON ||
        draw->CtlID != IDCANCEL) {
      handled = FALSE;
      return 0;
    }

    const auto scale = [&](int value) { return ScaleForDpi(dpi_, value); };
    const auto hot = (draw->itemState & ODS_HOTLIGHT) != 0;
    const auto pressed = (draw->itemState & ODS_SELECTED) != 0;

    COLORREF fill_color = hot
                              ? BlendColor(kSettingsDialogFooterBackground,
                                           kSettingsDialogAccent, 10)
                              : kSettingsDialogFooterBackground;
    COLORREF border_color = hot
                                ? BlendColor(kSettingsDialogBorder,
                                             kSettingsDialogAccent, 40)
                                : kSettingsDialogBorder;
    auto text_color = kSettingsDialogText;
    if (pressed) {
      fill_color = BlendColor(fill_color, RGB(32, 28, 24), 10);
      border_color = BlendColor(border_color, RGB(32, 28, 24), 12);
    }

    CDCHandle control_dc(draw->hDC);
    CRect rect(draw->rcItem);
    FillRoundedRect(control_dc, rect, fill_color, border_color, scale(12));

    const auto old_font = control_dc.SelectFont(button_font_);
    const auto old_mode = control_dc.SetBkMode(TRANSPARENT);
    const auto old_color = control_dc.SetTextColor(text_color);
    CRect text_rect(rect);
    control_dc.DrawText(L"Close", -1, &text_rect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    control_dc.SetTextColor(old_color);
    control_dc.SetBkMode(old_mode);
    control_dc.SelectFont(old_font);

    if ((draw->itemState & ODS_FOCUS) != 0) {
      CRect focus_rect(rect);
      focus_rect.DeflateRect(scale(4), scale(4));
      control_dc.DrawFocusRect(&focus_rect);
    }

    return TRUE;
  }

  LRESULT OnCtlColorDialog(UINT /*message*/, WPARAM w_param, LPARAM /*l_param*/,
                           BOOL& handled) {
    handled = TRUE;
    SetBkColor(reinterpret_cast<HDC>(w_param), kSettingsDialogBackground);
    return reinterpret_cast<LRESULT>(background_brush_.m_hBrush);
  }

  LRESULT OnCtlColorStatic(UINT /*message*/, WPARAM w_param, LPARAM l_param,
                           BOOL& handled) {
    handled = TRUE;
    const auto child_window = reinterpret_cast<HWND>(l_param);
    if (IsValueWindow(child_window)) {
      SetTextColor(reinterpret_cast<HDC>(w_param), kSettingsDialogText);
      SetBkColor(reinterpret_cast<HDC>(w_param), kSettingsDialogInputBackground);
      return reinterpret_cast<LRESULT>(input_brush_.m_hBrush);
    }

    auto text_color = kSettingsDialogText;
    if (child_window == eyebrow_.m_hWnd) {
      text_color = kSettingsDialogAccent;
    } else if (child_window == title_.m_hWnd) {
      text_color = kSettingsDialogText;
    } else if (child_window == title_label_.m_hWnd ||
               child_window == subject_label_.m_hWnd ||
               child_window == author_label_.m_hWnd ||
               child_window == signature_label_.m_hWnd ||
               child_window == size_label_.m_hWnd ||
               child_window == comment_label_.m_hWnd) {
      text_color = kSettingsDialogAccentDark;
    }

    SetTextColor(reinterpret_cast<HDC>(w_param), text_color);
    SetBkMode(reinterpret_cast<HDC>(w_param), TRANSPARENT);
    return reinterpret_cast<LRESULT>(GetSectionBrush(child_window));
  }

  LRESULT OnCtlColorEdit(UINT /*message*/, WPARAM w_param, LPARAM /*l_param*/,
                         BOOL& handled) {
    handled = TRUE;
    SetTextColor(reinterpret_cast<HDC>(w_param), kSettingsDialogText);
    SetBkColor(reinterpret_cast<HDC>(w_param), kSettingsDialogInputBackground);
    return reinterpret_cast<LRESULT>(input_brush_.m_hBrush);
  }

  void InitializeFonts() {
    const auto base_font = BuildBaseUiFont();
    const auto create_font = [&](CFont* font, int point_size, LONG weight) {
      LOGFONT face = base_font;
      face.lfHeight = -MulDiv(point_size, static_cast<int>(dpi_), 72);
      face.lfWeight = weight;
      face.lfQuality = CLEARTYPE_NATURAL_QUALITY;
      font->CreateFontIndirect(&face);
    };

    create_font(&heading_font_, 9, FW_SEMIBOLD);
    create_font(&title_font_, 22, FW_SEMIBOLD);
    create_font(&label_font_, 10, FW_SEMIBOLD);
    create_font(&body_font_, 10, FW_NORMAL);
    create_font(&button_font_, 10, FW_SEMIBOLD);
  }

  bool IsValueWindow(HWND window) const {
    return window == path_value_.m_hWnd || window == title_value_.m_hWnd ||
           window == subject_value_.m_hWnd || window == author_value_.m_hWnd ||
           window == signature_value_.m_hWnd || window == size_value_.m_hWnd ||
           window == comment_value_.m_hWnd;
  }

  HBRUSH GetSectionBrush(HWND child_window) const {
    if (child_window == nullptr) {
      return background_brush_.m_hBrush;
    }

    RECT child_rect{};
    if (!::GetWindowRect(child_window, &child_rect) ||
        !::MapWindowPoints(HWND_DESKTOP, m_hWnd,
                           reinterpret_cast<POINT*>(&child_rect), 2)) {
      return background_brush_.m_hBrush;
    }

    return child_rect.top >= ScaleForDpi(dpi_, 438) ? footer_brush_.m_hBrush
                                                    : panel_brush_.m_hBrush;
  }

  void EndModal(int result) {
    modal_result_ = result;
    if (IsWindow()) {
      DestroyWindow();
    }
  }

  FileDetailsModel details_;
  int modal_result_;
  UINT dpi_;
  CBrush background_brush_;
  CBrush panel_brush_;
  CBrush footer_brush_;
  CBrush input_brush_;
  CFont heading_font_;
  CFont title_font_;
  CFont label_font_;
  CFont body_font_;
  CFont button_font_;
  CWindow eyebrow_;
  CWindow title_;
  CWindow title_label_;
  CWindow subject_label_;
  CWindow author_label_;
  CWindow signature_label_;
  CWindow size_label_;
  CWindow comment_label_;
  CEdit path_value_;
  CEdit title_value_;
  CEdit subject_value_;
  CEdit author_value_;
  CEdit signature_value_;
  CEdit size_value_;
  CEdit comment_value_;
  CButton close_button_;
};

bool ShouldExcludeFile(const std::wstring& path,
                       const std::vector<std::wstring>& filters,
                       bool deep_scan_enabled) {
  if (filters.empty()) {
    return false;
  }

  for (const auto& filter : filters) {
    if (ContainsStringIgnoreCase(path, filter)) {
      return true;
    }
  }

  if (!deep_scan_enabled) {
    return false;
  }

  const auto metadata = BuildDeepScanMetadata(path);
  if (metadata.empty()) {
    return false;
  }

  for (const auto& filter : filters) {
    if (ContainsStringIgnoreCase(metadata, filter)) {
      return true;
    }
  }

  return false;
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
      excluded_size_(0),
      total_reclaimable_size_(0),
      selected_count_(0),
      excluded_count_(0),
      total_reclaimable_count_(0),
      sort_column_(0),
      hot_button_(0),
      pressed_button_(0),
      reveal_progress_(100),
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
      deep_scan_enabled_(true),
      missing_files_check_on_startup_(false),
      recovered_last_operation_(false),
      busy_operation_(BusyOperation::kNone),
      share_feedback_deadline_(0),
      hot_details_link_(false),
      pressed_details_link_(false) {
  ZeroMemory(&last_scan_time_, sizeof(last_scan_time_));
  details_link_rect_.SetRectEmpty();
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

  AppSettings settings;
  if (LoadAppSettings(&settings)) {
    deep_scan_enabled_ = settings.deep_scan_enabled;
    missing_files_check_on_startup_ = settings.missing_files_check_on_startup;
    exclusion_filters_ = settings.exclusion_filters;
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
  if (missing_files_check_on_startup_) {
    PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
  }

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
  UpdateHotDetailsLink(IsDetailsLinkEnabled() && details_link_rect_.PtInRect(point));
}

void MainFrame::OnMouseLeave() {
  tracking_mouse_ = false;
  UpdateHotButton(0);
  UpdateHotDetailsLink(false);
}

BOOL MainFrame::OnSetCursor(CWindow /*window*/, UINT hit_test,
                            UINT /*message*/) {
  if (hit_test == HTCLIENT) {
    POINT screen_point{};
    if (GetCursorPos(&screen_point)) {
      ScreenToClient(&screen_point);
      if (IsDetailsLinkEnabled() && details_link_rect_.PtInRect(screen_point)) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
    }
  }

  return FALSE;
}

void MainFrame::OnLButtonDown(UINT /*flags*/, CPoint point) {
  const auto command_id = GetButtonAtPoint(point);
  if (command_id != 0) {
    pressed_button_ = command_id;
    SetCapture();
    InvalidateRect(GetButtonRect(command_id), FALSE);
    return;
  }

  if (IsDetailsLinkEnabled() && details_link_rect_.PtInRect(point)) {
    pressed_details_link_ = true;
    SetCapture();
    InvalidateRect(details_link_rect_, FALSE);
    return;
  }

  SetMsgHandled(FALSE);
}

void MainFrame::OnLButtonUp(UINT /*flags*/, CPoint point) {
  const auto was_pressed = pressed_button_;
  const auto was_details_link_pressed = pressed_details_link_;
  if (GetCapture() == m_hWnd) {
    ReleaseCapture();
  }

  pressed_button_ = 0;
  pressed_details_link_ = false;
  if (was_pressed != 0) {
    InvalidateRect(GetButtonRect(was_pressed), FALSE);
    if (was_pressed == GetButtonAtPoint(point)) {
      InvokeSurfaceButton(was_pressed);
    }
    return;
  }

  if (was_details_link_pressed) {
    InvalidateRect(details_link_rect_, FALSE);
    if (IsDetailsLinkEnabled() && details_link_rect_.PtInRect(point)) {
      InvokeDetailsLink();
    }
  }
}

void MainFrame::OnTimer(UINT_PTR event_id) {
  if (event_id != kAnimationTimerId) {
    return;
  }

  auto share_feedback_expired = false;
  if (share_feedback_deadline_ != 0 &&
      GetTickCount64() >= share_feedback_deadline_) {
    share_feedback_deadline_ = 0;
    share_feedback_expired = true;
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
      moved_flash_changed || deleted_flash_changed || scan_flash_changed ||
      share_feedback_deadline_ != 0;

  if (!animating) {
    KillTimer(kAnimationTimerId);
  }

  if (reveal_changed) {
    LayoutChildren();
    Invalidate(FALSE);
    return;
  }
  if (action_changed) {
    InvalidateRect(action_rail_rect_, FALSE);
  }

  if (scan_flash_changed) {
    InvalidateRect(command_band_rect_, FALSE);
    InvalidateRect(list_frame_rect_, FALSE);
  }
  if (share_feedback_expired) {
    InvalidateRect(command_band_rect_, FALSE);
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

  excluded_count_ = 0;
  excluded_size_ = 0;
  if (scan_complete && !exclusion_filters_.empty()) {
    FileSizeMap visible_files;
    auto evaluated = 0;
    for (const auto& pair : files) {
      if (ShouldExcludeFile(pair.first, exclusion_filters_, deep_scan_enabled_)) {
        ++excluded_count_;
        excluded_size_ += pair.second;
      } else {
        visible_files.insert(pair);
      }

      ++evaluated;
      if ((evaluated % static_cast<int>(kResponsivePumpStride)) == 0) {
        PumpResponsiveUiMessages();
      }
    }
    files.swap(visible_files);
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
  if (file_list_.GetItemCount() > 0) {
    file_list_.SetItemState(0, LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
    file_list_.SetSelectionMark(0);
    file_list_.EnsureVisible(0, FALSE);
  }
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

void MainFrame::OnEditCopySummary(UINT /*notify_code*/, int /*id*/,
                                  CWindow /*control*/) {
  if (IsBusy() || !has_last_scan_ || !last_scan_succeeded_) {
    return;
  }

  TrackShareMetric(kShareEventClicked, total_reclaimable_count_,
                   total_reclaimable_size_, selected_count_, selected_size_,
                   last_scan_succeeded_);

  if (!CopyTextToClipboard(m_hWnd, BuildShareSummaryText())) {
    TrackShareMetric(kShareEventCopyFailed, total_reclaimable_count_,
                     total_reclaimable_size_, selected_count_, selected_size_,
                     last_scan_succeeded_);
    MessageBox(L"Patch Cleaner could not copy the share summary to the "
               L"clipboard.",
               L"Patch Cleaner", MB_ICONERROR | MB_OK);
    return;
  }

  TrackShareMetric(kShareEventCopied, total_reclaimable_count_,
                   total_reclaimable_size_, selected_count_, selected_size_,
                   last_scan_succeeded_);
  share_feedback_deadline_ = GetTickCount64() + kShareFeedbackDurationMs;
  scan_flash_ = 100;
  EnsureAnimationTimer();
  InvalidateRect(command_band_rect_, FALSE);
}

void MainFrame::OnAppSettings(UINT /*notify_code*/, int /*id*/,
                              CWindow /*control*/) {
  if (IsBusy()) {
    return;
  }

  AppSettings current_settings;
  current_settings.deep_scan_enabled = deep_scan_enabled_;
  current_settings.missing_files_check_on_startup =
      missing_files_check_on_startup_;
  current_settings.exclusion_filters = exclusion_filters_;

  SettingsDialog dialog(current_settings);
  if (dialog.DoModal(m_hWnd) != IDOK) {
    return;
  }

  if (!SaveAppSettings(dialog.settings())) {
    MessageBox(L"Patch Cleaner could not save the current settings.",
               L"Patch Cleaner", MB_ICONERROR | MB_OK);
    return;
  }

  deep_scan_enabled_ = dialog.settings().deep_scan_enabled;
  missing_files_check_on_startup_ =
      dialog.settings().missing_files_check_on_startup;
  exclusion_filters_ = dialog.settings().exclusion_filters;
  InvalidateRect(command_band_rect_, FALSE);

  if (has_last_scan_) {
    PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
  }
}

void MainFrame::OnViewFileDetails(UINT /*notify_code*/, int /*id*/,
                                  CWindow /*control*/) {
  if (!IsDetailsLinkEnabled()) {
    return;
  }

  const auto item_index = GetDetailsTargetIndex();
  if (item_index < 0) {
    MessageBox(L"Select an installer result before opening details.",
               L"Patch Cleaner", MB_ICONINFORMATION | MB_OK);
    return;
  }

  const auto* file_item =
      reinterpret_cast<const FileItem*>(file_list_.GetItemData(item_index));
  if (file_item == nullptr) {
    MessageBox(L"Patch Cleaner could not read the selected installer details.",
               L"Patch Cleaner", MB_ICONERROR | MB_OK);
    return;
  }

  FileDetailsDialog dialog(BuildFileDetailsModel(*file_item));
  dialog.DoModal(m_hWnd);
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
    share_feedback_deadline_ = 0;
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
  const auto action_height = ScaleForDpi(dpi_, 96);

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
  const auto summary_width = ScaleForDpi(dpi_, 148);
  const auto scan_top = command_band_rect_.top + ScaleForDpi(dpi_, 42);
  scan_button_rect_.SetRect(command_band_rect_.right - button_padding_right -
                                scan_width,
                            scan_top,
                            command_band_rect_.right - button_padding_right,
                            scan_top + button_height);
  copy_summary_button_rect_.SetRect(
      command_band_rect_.right - button_padding_right - summary_width,
      scan_button_rect_.bottom + ScaleForDpi(dpi_, 10),
      command_band_rect_.right - button_padding_right,
      scan_button_rect_.bottom + ScaleForDpi(dpi_, 10) + button_height);

  const auto delete_width = ScaleForDpi(dpi_, 114);
  const auto move_width = ScaleForDpi(dpi_, 156);
  const auto select_width = ScaleForDpi(dpi_, 122);
  const auto settings_width = ScaleForDpi(dpi_, 112);
  const auto button_top = action_rail_rect_.bottom - ScaleForDpi(dpi_, 22) -
                          button_height;

  settings_button_rect_.SetRect(action_rail_rect_.right - button_padding_right -
                                    settings_width,
                              button_top,
                              action_rail_rect_.right - button_padding_right,
                              button_top + button_height);
  delete_button_rect_.SetRect(settings_button_rect_.left - button_gap -
                                  delete_width,
                              button_top,
                              settings_button_rect_.left - button_gap,
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
  reveal_progress_ = 100;
}

void MainFrame::EnsureAnimationTimer() {
  const auto action_target = selected_count_ > 0 ? 100 : 0;
  const auto should_run = reveal_progress_ < 100 ||
                          action_progress_ != action_target ||
                          selected_flash_ > 0 || moved_flash_ > 0 ||
                          deleted_flash_ > 0 || scan_flash_ > 0 ||
                          share_feedback_deadline_ != 0;
  if (should_run) {
    SetTimer(kAnimationTimerId, 15);
  } else {
    KillTimer(kAnimationTimerId);
  }
}

int MainFrame::GetButtonAtPoint(CPoint point) const {
  const std::array<int, 6> buttons{{
      kButtonScan,
      kButtonCopySummary,
      kButtonSettings,
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
    case kButtonCopySummary:
      return copy_summary_button_rect_;
    case kButtonSettings:
      return settings_button_rect_;
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
    case kButtonCopySummary:
      return has_last_scan_ && last_scan_succeeded_;
    case kButtonSettings:
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

bool MainFrame::IsDetailsLinkEnabled() const {
  return !IsBusy() && share_feedback_deadline_ == 0 && has_last_scan_ &&
         last_scan_succeeded_ && file_list_.IsWindow() &&
         file_list_.GetItemCount() > 0;
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

void MainFrame::UpdateHotDetailsLink(bool hot) {
  if (hot_details_link_ == hot) {
    return;
  }

  hot_details_link_ = hot;
  if (!details_link_rect_.IsRectEmpty()) {
    InvalidateRect(details_link_rect_, FALSE);
  }
}

void MainFrame::InvokeSurfaceButton(int command_id) {
  if (!IsButtonEnabled(command_id)) {
    return;
  }

  SendMessage(WM_COMMAND, MAKEWPARAM(command_id, 0), 0);
}

void MainFrame::InvokeDetailsLink() {
  if (!IsDetailsLinkEnabled()) {
    return;
  }

  SendMessage(WM_COMMAND, MAKEWPARAM(ID_VIEW_FILE_DETAILS, 0), 0);
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
    case kButtonCopySummary:
      label = L"Copy Summary";
      break;
    case kButtonSettings:
      label = L"Settings";
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

  details_link_rect_.SetRectEmpty();
  FillRectColor(dc, client_rect, kWindowBackground);
  PaintCommandBand(dc, command_band_rect_);
  PaintListFrame(dc, list_frame_rect_);
  PaintActionRail(dc, action_rail_rect_);
}

void MainFrame::PaintCommandBand(CDCHandle dc, const CRect& rect) {
  FillRectColor(dc, rect, kCommandBandBackground);

  const auto old_mode = dc.SetBkMode(TRANSPARENT);
  auto text_rect = rect;
  text_rect.left += ScaleForDpi(dpi_, 28);
  text_rect.top += ScaleForDpi(dpi_, 22);
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
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

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
  DrawSurfaceButton(dc, copy_summary_button_rect_, kButtonCopySummary);

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

void MainFrame::PaintActionRail(CDCHandle dc, const CRect& rect) {
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
  auto state_text_rect = state_rect;
  if (IsDetailsLinkEnabled()) {
    CString link_text(L"details...");
    CRect link_rect(state_rect);
    dc.DrawText(link_text, link_text.GetLength(), &link_rect,
                DT_CALCRECT | DT_SINGLELINE);
    const auto link_width = link_rect.Width();

    SIZE selection_size{};
    dc.GetTextExtent(selection_line, selection_line.GetLength(), &selection_size);

    const auto link_gap = ScaleForDpi(dpi_, 10);
    const auto preferred_left = state_rect.left + selection_size.cx + link_gap;
    link_rect.left =
        std::min(std::max(state_rect.left + ScaleForDpi(dpi_, 120),
                          preferred_left),
                 std::max(state_rect.left + ScaleForDpi(dpi_, 120),
                          state_rect.right - link_width));
    link_rect.right = std::min(state_rect.right, link_rect.left + link_width);
    state_text_rect.right =
        std::max(state_text_rect.left, link_rect.left - link_gap);
    dc.DrawText(selection_line, selection_line.GetLength(), &state_text_rect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    const auto link_color =
        pressed_details_link_
            ? kAccentDarkColor
            : (hot_details_link_ ? BlendColor(kAccentDarkColor, kInkColor, 20)
                                 : kAccentColor);
    dc.SetTextColor(link_color);
    dc.DrawText(link_text, link_text.GetLength(), &link_rect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    CPen underline_pen;
    underline_pen.CreatePen(PS_SOLID, 1, link_color);
    const auto old_pen = dc.SelectPen(underline_pen);
    const auto underline_y = link_rect.bottom - ScaleForDpi(dpi_, 3);
    dc.MoveTo(link_rect.left, underline_y);
    dc.LineTo(link_rect.right, underline_y);
    dc.SelectPen(old_pen);

    details_link_rect_ = link_rect;
    details_link_rect_.InflateRect(ScaleForDpi(dpi_, 3), ScaleForDpi(dpi_, 2));
    dc.SetTextColor(old_color);
  } else {
    dc.DrawText(selection_line, selection_line.GetLength(), &state_text_rect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  }

  auto detail_rect = text_rect;
  detail_rect.top = state_rect.bottom + ScaleForDpi(dpi_, 6);
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
  DrawSurfaceButton(dc, settings_button_rect_, kButtonSettings);

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

  if (share_feedback_deadline_ != 0) {
    return CString(L"Share-ready summary copied to clipboard");
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
    if (excluded_count_ > 0) {
      CString excluded_text;
      excluded_text.Format(L" | %d excluded by filters", excluded_count_);
      state_line.Append(excluded_text);
    }
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

  if (share_feedback_deadline_ != 0) {
    return CString(L"Paste it into email, Slack, forums, or a social post "
                   L"with the Patch Cleaner link included.");
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
  if (excluded_count_ > 0) {
    detail_line.Format(L"%s reclaimable after excluding %d file%s (%s).",
                       static_cast<LPCWSTR>(
                           FormatSizeString(total_reclaimable_size_)),
                       excluded_count_, excluded_count_ == 1 ? L"" : L"s",
                       static_cast<LPCWSTR>(FormatSizeString(excluded_size_)));
  } else if (!exclusion_filters_.empty() && !deep_scan_enabled_) {
    detail_line.Append(
        L" Deep Scan is off, so filters currently match file paths only.");
  }
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

std::wstring MainFrame::BuildShareSummaryText() const {
  CString summary;
  if (has_last_scan_ && last_scan_succeeded_) {
    summary.Format(
        L"Patch Cleaner found %d reclaimable Windows Installer file%s (%s) "
        L"on this PC.",
        total_reclaimable_count_, total_reclaimable_count_ == 1 ? L"" : L"s",
        static_cast<LPCWSTR>(FormatSizeString(total_reclaimable_size_)));

    if (selected_count_ > 0) {
      CString selection_summary;
      selection_summary.Format(L" I have %d file%s (%s) selected for cleanup.",
                               selected_count_,
                               selected_count_ == 1 ? L"" : L"s",
                               static_cast<LPCWSTR>(
                                   FormatSizeString(selected_size_)));
      summary.Append(selection_summary);
    } else {
      summary.Append(L" Reviewing the leftovers before cleaning them up.");
    }
  } else {
    summary.Append(
        L"I'm using Patch Cleaner to review leftover Windows Installer files "
        L"before cleanup.");
  }

  const auto cleaned_size = moved_size_ + deleted_size_;
  if (cleaned_size > 0) {
    CString cleaned_summary;
    cleaned_summary.Format(L" Cleaned %s so far.",
                           static_cast<LPCWSTR>(FormatSizeString(cleaned_size)));
    summary.Append(cleaned_summary);
  }

  summary.Append(L" ");
  summary.Append(kShareProductUrl);
  return std::wstring(summary.GetString());
}

int MainFrame::GetDetailsTargetIndex() const {
  if (!file_list_.IsWindow()) {
    return -1;
  }

  auto& list = const_cast<CListViewCtrl&>(file_list_);
  auto item_index = list.GetNextItem(-1, LVNI_SELECTED);
  if (item_index < 0) {
    item_index = list.GetSelectionMark();
  }
  if (item_index < 0) {
    item_index = list.GetNextItem(-1, LVNI_FOCUSED);
  }
  if (item_index < 0 && list.GetItemCount() > 0) {
    item_index = 0;
  }

  return item_index;
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
