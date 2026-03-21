// Copyright (c) 2016 dacci.org

#include "ui/main_frame.h"

#include <atlstr.h>

#include <msi.h>
#include <shlobj.h>

#include <map>
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

constexpr wchar_t kAllUsersSid[] = L"s-1-1-0";
constexpr wchar_t kTempMoveDirectory[] = L"C:\\TempPatchCleanerFiles";
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

void EnumFiles(const std::wstring& base_path, const wchar_t* pattern,
               FileSizeMap* output) {
  auto query = base_path + pattern;
  WIN32_FIND_DATA find_data;
  auto find = FindFirstFile(query.c_str(), &find_data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      auto path = base_path + find_data.cFileName;

      ULARGE_INTEGER size;
      size.LowPart = find_data.nFileSizeLow;
      size.HighPart = find_data.nFileSizeHigh;

      output->insert({path, size.QuadPart});
    } while (FindNextFile(find, &find_data));

    FindClose(find);
  }
}

template <size_t length>
int FormatSize(uint64_t size, wchar_t (&buffer)[length]) {
  static const wchar_t* kUnits[]{L"B", L"KB", L"MB", L"GB", L"TB"};

  auto double_size = static_cast<double>(size);
  auto index = 0;
  for (; double_size >= 1024.0 && index < _countof(kUnits) - 1; ++index)
    double_size /= 1024.0;

  return swprintf_s(buffer, L"%.1lf %s", double_size, kUnits[index]);
}

double ToMegabytes(uint64_t size) {
  return static_cast<double>(size) / (1024.0 * 1024.0);
}

bool EnsureDirectoryExists(const wchar_t* path) {
  if (CreateDirectory(path, nullptr)) {
    return true;
  }

  return GetLastError() == ERROR_ALREADY_EXISTS;
}

template <typename Action>
bool ChangeFileWithWritableAttributes(const wchar_t* path, Action action,
                                      bool* mutation_attempted = nullptr) {
  if (mutation_attempted != nullptr) {
    *mutation_attempted = false;
  }

  auto attributes = GetFileAttributes(path);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }

  const auto was_read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
  if (was_read_only &&
      SetFileAttributes(path, attributes & ~FILE_ATTRIBUTE_READONLY) == FALSE) {
    return false;
  }

  if (action()) {
    if (mutation_attempted != nullptr) {
      *mutation_attempted = true;
    }
    return true;
  }

  if (mutation_attempted != nullptr) {
    *mutation_attempted = true;
  }

  if (was_read_only) {
    SetFileAttributes(path, attributes);
  }

  return false;
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

    const auto remaining = failure_count - static_cast<int>(failure_samples.size());
    if (remaining > 0) {
      CString remaining_text;
      remaining_text.Format(L"\n - ...and %d more.", remaining);
      message.Append(remaining_text);
    }
  }

  return message;
}

std::wstring GetFileNameFromPath(const std::wstring& path) {
  const auto separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return path;
  }

  return path.substr(separator + 1);
}

std::wstring BuildMoveCandidatePath(const std::wstring& file_name, int suffix) {
  if (suffix == 0) {
    return std::wstring{kTempMoveDirectory}.append(L"\\").append(file_name);
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

  std::wstring candidate{kTempMoveDirectory};
  candidate.append(L"\\").append(base_name);
  candidate.append(suffix_text.GetString());
  candidate.append(extension_name);
  return candidate;
}

std::wstring BuildUniqueMovePath(const std::wstring& source_path) {
  const auto file_name = GetFileNameFromPath(source_path);
  for (int suffix = 0;; ++suffix) {
    auto candidate = BuildMoveCandidatePath(file_name, suffix);
    auto attributes = GetFileAttributes(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      auto error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND) {
        return candidate;
      }

      return std::wstring();
    }
  }
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
  if (CFrameWindowImpl::PreTranslateMessage(message))
    return TRUE;

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

      if (new_checked)
        selected_size_ += file_item->size;
      else
        selected_size_ -= file_item->size;

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
  {
    PWSTR windir = nullptr;
    auto result =
        SHGetKnownFolderPath(FOLDERID_Windows, KF_FLAG_DEFAULT, NULL, &windir);
    if (FAILED(result)) {
      return;
    }

    cache_path.assign(windir).append(L"\\Installer\\");
    CoTaskMemFree(windir);
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
  for (auto i = 0, ix = file_list_.GetItemCount(); i < ix; ++i)
    file_list_.SetCheckState(i, TRUE);
}

void MainFrame::OnFileMoveToTemp(UINT /*notify_code*/, int /*id*/,
                                 CWindow /*control*/) {
  if (!EnsureDirectoryExists(kTempMoveDirectory)) {
    MessageBox(L"Could not create C:\\TempPatchCleanerFiles.", L"Patch Cleaner",
               MB_ICONERROR | MB_OK);
    return;
  }

  uint64_t moved_this_run = 0;
  int move_failure_count = 0;
  std::vector<std::wstring> move_failure_samples;
  auto destination_error = false;
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

    auto destination = BuildUniqueMovePath(file_item->path);
    if (destination.empty()) {
      destination_error = true;
      break;
    }

    auto mutation_attempted = false;
    if (!ChangeFileWithWritableAttributes(
            file_item->path.c_str(),
            [&] {
              return MoveFileEx(file_item->path.c_str(), destination.c_str(),
                                MOVEFILE_COPY_ALLOWED |
                                    MOVEFILE_WRITE_THROUGH) != FALSE;
            },
            &mutation_attempted)) {
      if (mutation_attempted) {
        RecordFailurePath(file_item->path, &move_failure_count,
                          &move_failure_samples);
      }
      continue;
    }

    moved_this_run += file_item->size;
  }

  moved_size_ += moved_this_run;
  PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
  if (destination_error) {
    MessageBox(L"Could not access C:\\TempPatchCleanerFiles.", L"Patch Cleaner",
               MB_ICONERROR | MB_OK);
  }
  if (move_failure_count > 0) {
    auto message = BuildFailureMessage(L"Move to Temp", move_failure_count,
                                       move_failure_samples);
    MessageBox(message, L"Patch Cleaner", MB_ICONWARNING | MB_OK);
  }
}

void MainFrame::OnEditDelete(UINT /*notify_code*/, int /*id*/,
                             CWindow /*control*/) {
  uint64_t deleted_this_run = 0;
  int delete_failure_count = 0;
  std::vector<std::wstring> delete_failure_samples;
  for (auto i = 0, ix = file_list_.GetItemCount(); i < ix; ++i) {
    auto state = file_list_.GetItemState(i, LVIS_STATEIMAGEMASK);
    if ((state & INDEXTOSTATEIMAGEMASK(2)) == 0)
      continue;

    auto* file_item =
        reinterpret_cast<FileItem*>(file_list_.GetItemData(i));
    if (file_item == nullptr) {
      continue;
    }

    auto mutation_attempted = false;
    if (!ChangeFileWithWritableAttributes(
            file_item->path.c_str(),
            [&] { return DeleteFile(file_item->path.c_str()) != FALSE; },
            &mutation_attempted)) {
      if (mutation_attempted) {
        RecordFailurePath(file_item->path, &delete_failure_count,
                          &delete_failure_samples);
      }
      continue;
    }

    deleted_this_run += file_item->size;
  }

  deleted_size_ += deleted_this_run;
  PostMessage(WM_COMMAND, MAKEWPARAM(ID_FILE_UPDATE, 0));
  if (delete_failure_count > 0) {
    auto message = BuildFailureMessage(L"Delete", delete_failure_count,
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

}  // namespace ui
}  // namespace patch_cleaner
