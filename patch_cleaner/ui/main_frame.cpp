// Copyright (c) 2016 dacci.org

#include "ui/main_frame.h"

#include <atlstr.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <msi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <map>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "app/application.h"

namespace patch_cleaner {
namespace ui {

namespace {

typedef CWinTraitsOR<TBSTYLE_TOOLTIPS | TBSTYLE_LIST, TBSTYLE_EX_MIXEDBUTTONS>
    ToolBarWinTraits;
typedef CWinTraitsOR<SBARS_SIZEGRIP> StatusBarWinTraits;
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

constexpr wchar_t kAllUsersSid[] = L"s-1-1-0";
constexpr wchar_t kTempMoveDirectory[] = L"C:\\TempPatchCleanerFiles";
constexpr wchar_t kOperationArgument[] = L"--elevated-operation";
constexpr wchar_t kOperationRootSuffix[] = L"\\PatchCleaner\\Operations";
constexpr wchar_t kOperationMoveToken[] = L"move";
constexpr wchar_t kOperationDeleteToken[] = L"delete";
constexpr wchar_t kResultSuccessPrefix[] = L"ok\t";
constexpr wchar_t kResultFailurePrefix[] = L"fail\t";
constexpr wchar_t kResultDestinationError[] = L"destination_error";
constexpr wchar_t kSecureSubdirectorySddl[] =
    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
constexpr size_t kMaxFailureSamples = 5;

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
         written == size_in_bytes;
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

bool WriteOperationRequest(MutationOperation operation,
                           const std::vector<std::wstring>& paths,
                           std::wstring* request_path,
                           std::wstring* result_path) {
  std::wstring operation_directory;
  if (!GetOperationDirectory(&operation_directory) ||
      !EnsureDirectoryChainExists(operation_directory)) {
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

  return WriteWideLinesFile(result_path, lines, false);
}

bool ReadOperationReply(const std::wstring& result_path, MutationReply* reply) {
  std::vector<std::wstring> lines;
  if (!ReadWideLinesFile(result_path, &lines)) {
    return false;
  }

  reply->destination_error = false;
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
    }
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

  const auto cleanup = [&]() {
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

  WaitForSingleObject(execute_info.hProcess, INFINITE);
  CloseHandle(execute_info.hProcess);

  const auto read_success = ReadOperationReply(result_path, reply);
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

bool CreateSecureMoveSubdirectory(const std::wstring& root_directory,
                                  std::wstring* subdirectory_path,
                                  CHandle* subdirectory_handle) {
  SECURITY_ATTRIBUTES attributes{};
  std::unique_ptr<void, LocalMemDeleter> descriptor_holder;
  if (!BuildSecureDirectoryAttributes(&attributes, &descriptor_holder)) {
    return false;
  }

  for (int attempt = 0; attempt < 8; ++attempt) {
    std::wstring random_suffix;
    if (!GenerateRandomHexString(16, &random_suffix)) {
      return false;
    }

    std::wstring candidate_path = root_directory;
    candidate_path.append(L"\\").append(random_suffix);
    if (!CreateDirectoryW(candidate_path.c_str(), &attributes)) {
      if (GetLastError() == ERROR_ALREADY_EXISTS) {
        continue;
      }
      return false;
    }

    if (!OpenValidatedDirectory(candidate_path,
                                FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE,
                                subdirectory_handle)) {
      RemoveDirectoryW(candidate_path.c_str());
      return false;
    }

    *subdirectory_path = candidate_path;
    return true;
  }

  return false;
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
    SetFileInformationByHandle(handle, FileBasicInfo, &original_info,
                               sizeof(original_info));
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

std::wstring BuildMoveCandidateName(const std::wstring& file_name, int suffix) {
  if (suffix == 0) {
    return file_name;
  }

  const auto extension = file_name.find_last_of(L'.');
  std::wstring base_name = file_name;
  std::wstring extension_name;
  if (extension != std::wstring::npos) {
    base_name = file_name.substr(0, extension);
    extension_name = file_name.substr(extension);
  }

  CString suffix_text;
  suffix_text.Format(L" (%d)", suffix);
  return base_name + std::wstring(suffix_text.GetString()) + extension_name;
}

bool BuildUniqueMoveName(const std::wstring& destination_directory,
                         const std::wstring& source_path,
                         std::wstring* destination_name) {
  const auto file_name = GetFileNameFromPath(source_path);
  for (int suffix = 0;; ++suffix) {
    auto candidate_name = BuildMoveCandidateName(file_name, suffix);
    auto candidate_path = destination_directory;
    candidate_path.append(L"\\").append(candidate_name);

    const auto attributes = GetFileAttributesW(candidate_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        *destination_name = std::move(candidate_name);
        return true;
      }
      return false;
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

bool MoveInstallerCacheFile(const std::wstring& path,
                            const std::wstring& installer_directory,
                            const std::wstring& destination_directory,
                            HANDLE destination_directory_handle) {
  CHandle file_handle;
  FILE_BASIC_INFO original_info{};
  auto attributes_changed = false;
  if (!OpenValidatedInstallerFile(path, installer_directory, &file_handle) ||
      !PrepareFileForMutation(file_handle, &original_info,
                              &attributes_changed)) {
    return false;
  }

  std::wstring destination_name;
  if (!BuildUniqueMoveName(destination_directory, path, &destination_name)) {
    return false;
  }

  std::wstring final_source_path;
  if (!GetPathFromHandle(file_handle, &final_source_path)) {
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
    if (CopyOpenedFileToDestination(file_handle, destination_path) &&
        DeleteOpenedFile(file_handle)) {
      return true;
    }

    DeleteFileW(destination_path.c_str());
    RestoreFileAttributesIfNeeded(file_handle, original_info, attributes_changed);
    return false;
  }

  if (RenameOpenedFileIntoDirectory(file_handle, destination_directory_handle,
                                    destination_name)) {
    return true;
  }

  RestoreFileAttributesIfNeeded(file_handle, original_info, attributes_changed);
  return false;
}

bool ExecuteDeleteOperation(const std::vector<std::wstring>& paths,
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
  }

  return true;
}

bool ExecuteMoveOperation(const std::vector<std::wstring>& paths,
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

  std::wstring secure_subdirectory;
  CHandle secure_subdirectory_handle;
  if (!CreateSecureMoveSubdirectory(kTempMoveDirectory, &secure_subdirectory,
                                    &secure_subdirectory_handle)) {
    reply->destination_error = true;
    return false;
  }

  for (const auto& path : paths) {
    if (MoveInstallerCacheFile(path, installer_directory, secure_subdirectory,
                               secure_subdirectory_handle)) {
      reply->succeeded_paths.push_back(path);
    } else {
      reply->failed_paths.push_back(path);
    }
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

double ToMegabytes(uint64_t size) {
  return static_cast<double>(size) / (1024.0 * 1024.0);
}

void EnumFiles(const std::wstring& base_path, const wchar_t* pattern,
               FileSizeMap* output) {
  auto query = base_path + pattern;
  WIN32_FIND_DATAW find_data{};
  auto find = FindFirstFileW(query.c_str(), &find_data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }

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
  }

  return true;
}

}  // namespace

MainFrame::MainFrame()
    : selected_size_(0),
      moved_size_(0),
      deleted_size_(0),
      sort_column_(0),
      sort_ascending_(true) {}

BOOL MainFrame::PreTranslateMessage(MSG* message) {
  if (CFrameWindowImpl::PreTranslateMessage(message)) {
    return TRUE;
  }

  return FALSE;
}

int MainFrame::OnCreate(CREATESTRUCT* /*create*/) {
  if (!app::GetApplication()->GetMessageLoop()->AddMessageFilter(this)) {
    return -1;
  }

  CBitmap tool_bar_bitmap;
  tool_bar_bitmap = AtlLoadBitmapImage(IDR_MAIN, LR_CREATEDIBSECTION);
  if (tool_bar_bitmap.IsNull()) {
    return -1;
  }

  if (!tool_bar_image_.Create(16, 16, ILC_COLOR32, 0, 2)) {
    return -1;
  }

  if (tool_bar_image_.Add(tool_bar_bitmap) == -1) {
    return -1;
  }

  m_hWndToolBar = tool_bar_.Create(
      m_hWnd, nullptr, nullptr, ToolBarWinTraits::GetWndStyle(0),
      ToolBarWinTraits::GetWndExStyle(0), ATL_IDW_TOOLBAR);
  if (tool_bar_.IsWindow()) {
    tool_bar_.SetImageList(tool_bar_image_);
    tool_bar_.AddString(ID_FILE_UPDATE);
    tool_bar_.AddString(ID_EDIT_DELETE);
  } else {
    return -1;
  }

  tool_bar_.AddButton(ID_FILE_UPDATE, BTNS_AUTOSIZE | BTNS_SHOWTEXT,
                      TBSTATE_ENABLED, 0, MAKEINTRESOURCE(0), 0);
  tool_bar_.AddButton(ID_EDIT_SELECT_ALL, BTNS_AUTOSIZE | BTNS_SHOWTEXT,
                      TBSTATE_ENABLED, I_IMAGENONE, L"Select All", 0);
  tool_bar_.AddButton(ID_FILE_MOVE_TO_TEMP, BTNS_AUTOSIZE | BTNS_SHOWTEXT,
                      TBSTATE_ENABLED, I_IMAGENONE, L"Move to Temp", 0);
  tool_bar_.AddButton(ID_EDIT_DELETE, BTNS_AUTOSIZE | BTNS_SHOWTEXT,
                      TBSTATE_ENABLED, 1, MAKEINTRESOURCE(1), 0);

  m_hWndStatusBar = status_bar_.Create(m_hWnd, nullptr, nullptr,
                                       StatusBarWinTraits::GetWndStyle(0),
                                       StatusBarWinTraits::GetWndExStyle(0));
  if (!status_bar_.IsWindow()) {
    return -1;
  }

  m_hWndClient = file_list_.Create(
      m_hWnd, nullptr, nullptr, FileListWinTraits::GetWndStyle(0),
      FileListWinTraits::GetWndExStyle(0), IDC_FILE_LIST);
  if (file_list_.IsWindow()) {
    file_list_.SetExtendedListViewStyle(
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
        LVS_EX_SIMPLESELECT | LVS_EX_AUTOSIZECOLUMNS);

    LVCOLUMN column{LVCF_FMT | LVCF_WIDTH | LVCF_TEXT};
    column.fmt = LVCFMT_LEFT;
    column.cx = 250;
    column.pszText = L"Path";
    file_list_.InsertColumn(0, &column);

    column.fmt = LVCFMT_RIGHT;
    column.cx = 80;
    column.pszText = L"Size";
    file_list_.InsertColumn(1, &column);
  } else {
    return -1;
  }

  RefreshStatusBar();

  return 0;
}

void MainFrame::OnDestroy() {
  SetMsgHandled(FALSE);

  app::GetApplication()->GetMessageLoop()->RemoveMessageFilter(this);
}

LRESULT MainFrame::OnItemChanged(NMHDR* header) {
  auto data = reinterpret_cast<NMLISTVIEW*>(header);

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
      } else {
        selected_size_ -= file_item->size;
      }

      RefreshStatusBar();
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
  return 0;
}

LRESULT MainFrame::OnDeleteItem(NMHDR* header) {
  auto data = reinterpret_cast<NMLISTVIEW*>(header);
  delete reinterpret_cast<FileItem*>(data->lParam);

  return 0;
}

void MainFrame::OnFileUpdate(UINT /*notify_code*/, int /*id*/,
                             CWindow /*control*/) {
  std::wstring cache_path;
  if (!GetInstallerDirectory(&cache_path)) {
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
  }

  selected_size_ = 0;
  ApplySort();
  RefreshStatusBar();

  file_list_.SetRedraw(TRUE);
  file_list_.RedrawWindow(
      NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);

  if (!scan_complete) {
    MessageBox(
        L"Patch Cleaner could not complete the installer scan safely, so no "
        L"files were listed.",
        L"Patch Cleaner", MB_ICONERROR | MB_OK);
  }
}

void MainFrame::OnEditSelectAll(UINT /*notify_code*/, int /*id*/,
                                CWindow /*control*/) {
  for (auto i = 0, ix = file_list_.GetItemCount(); i < ix; ++i) {
    file_list_.SetCheckState(i, TRUE);
  }
}

void MainFrame::OnFileMoveToTemp(UINT /*notify_code*/, int /*id*/,
                                 CWindow /*control*/) {
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

  MutationReply reply;
  bool canceled = false;
  if (!LaunchElevatedOperation(MutationOperation::kMoveToTemp, paths, &reply,
                               &canceled)) {
    if (!canceled) {
      MessageBox(L"Patch Cleaner could not complete the elevated move request.",
                 L"Patch Cleaner", MB_ICONERROR | MB_OK);
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
  PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));

  if (reply.destination_error) {
    MessageBox(L"Could not access C:\\TempPatchCleanerFiles securely.",
               L"Patch Cleaner", MB_ICONERROR | MB_OK);
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

  MutationReply reply;
  bool canceled = false;
  if (!LaunchElevatedOperation(MutationOperation::kDelete, paths, &reply,
                               &canceled)) {
    if (!canceled) {
      MessageBox(L"Patch Cleaner could not complete the elevated delete "
                 L"request.",
                 L"Patch Cleaner", MB_ICONERROR | MB_OK);
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
  PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));

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

void MainFrame::RefreshStatusBar() {
  CString status_text;
  status_text.Format(
      L"Selected: %.2f MB | Moved: %.2f MB | Deleted: %.2f MB",
      ToMegabytes(selected_size_), ToMegabytes(moved_size_),
      ToMegabytes(deleted_size_));
  status_bar_.SetText(0, status_text);
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

  MutationReply reply;
  const auto success =
      operation == MutationOperation::kMoveToTemp
          ? ExecuteMoveOperation(paths, &reply)
          : ExecuteDeleteOperation(paths, &reply);

  if (!WriteOperationReply(BuildResultPath(request_path), reply)) {
    return GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT
                                           : static_cast<int>(GetLastError());
  }

  return success ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

}  // namespace ui
}  // namespace patch_cleaner
