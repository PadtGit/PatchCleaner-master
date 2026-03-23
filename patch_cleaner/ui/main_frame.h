// Copyright (c) 2016 dacci.org

#ifndef PATCH_CLEANER_UI_MAIN_FRAME_H_
#define PATCH_CLEANER_UI_MAIN_FRAME_H_

#include <stdint.h>

#pragma warning(push, 3)
#include <atlbase.h>
#include <atlwin.h>

#include <atlapp.h>
#include <atlcrack.h>
#include <atlctrls.h>
#include <atlframe.h>
#include <atlstr.h>
#include <atltypes.h>
#pragma warning(pop)

#include "res/resource.h"

#ifndef ID_FILE_MOVE_TO_TEMP
#define ID_FILE_MOVE_TO_TEMP 40001
#endif

namespace patch_cleaner {
namespace ui {

int RunElevatedOperationRequest(const wchar_t* request_path);

class MainFrame : public CFrameWindowImpl<MainFrame>, private CMessageFilter {
 public:
  MainFrame();

  DECLARE_FRAME_WND_CLASS(L"MainFrame", IDR_MAIN)

  BEGIN_MSG_MAP(MainFrame)
    MSG_WM_CREATE(OnCreate)
    MSG_WM_DESTROY(OnDestroy)
    MSG_WM_SIZE(OnSize)
    MSG_WM_PAINT(OnPaint)
    MSG_WM_ERASEBKGND(OnEraseBkgnd)
    MSG_WM_MOUSEMOVE(OnMouseMove)
    MSG_WM_MOUSELEAVE(OnMouseLeave)
    MSG_WM_LBUTTONDOWN(OnLButtonDown)
    MSG_WM_LBUTTONUP(OnLButtonUp)
    MSG_WM_TIMER(OnTimer)
    MSG_WM_SETFOCUS(OnSetFocus)
    MSG_WM_GETMINMAXINFO(OnGetMinMaxInfo)

    NOTIFY_HANDLER_EX(IDC_FILE_LIST, LVN_ITEMCHANGED, OnItemChanged)
    NOTIFY_HANDLER_EX(IDC_FILE_LIST, LVN_COLUMNCLICK, OnColumnClick)
    NOTIFY_HANDLER_EX(IDC_FILE_LIST, LVN_DELETEITEM, OnDeleteItem)
    NOTIFY_CODE_HANDLER_EX(NM_CUSTOMDRAW, OnCustomDraw)

    COMMAND_ID_HANDLER_EX(ID_FILE_UPDATE, OnFileUpdate)
    COMMAND_ID_HANDLER_EX(ID_EDIT_SELECT_ALL, OnEditSelectAll)
    COMMAND_ID_HANDLER_EX(ID_FILE_MOVE_TO_TEMP, OnFileMoveToTemp)
    COMMAND_ID_HANDLER_EX(ID_EDIT_DELETE, OnEditDelete)

    MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
    CHAIN_MSG_MAP(CFrameWindowImpl)
  END_MSG_MAP()

 private:
  enum Controls {
    IDC_FILE_LIST = 1,
  };
  enum SurfaceButtons {
    kButtonScan = ID_FILE_UPDATE,
    kButtonSelectAll = ID_EDIT_SELECT_ALL,
    kButtonMoveToTemp = ID_FILE_MOVE_TO_TEMP,
    kButtonDelete = ID_EDIT_DELETE,
  };

  BOOL PreTranslateMessage(MSG* message) override;

  int OnCreate(CREATESTRUCT* create);
  void OnDestroy();
  void OnSize(UINT type, CSize size);
  void OnPaint(CDCHandle dc);
  BOOL OnEraseBkgnd(CDCHandle dc);
  void OnMouseMove(UINT flags, CPoint point);
  void OnMouseLeave();
  void OnLButtonDown(UINT flags, CPoint point);
  void OnLButtonUp(UINT flags, CPoint point);
  void OnTimer(UINT_PTR event_id);
  void OnSetFocus(CWindow old_window);
  void OnGetMinMaxInfo(MINMAXINFO* min_max_info);

  LRESULT OnItemChanged(NMHDR* header);
  LRESULT OnColumnClick(NMHDR* header);
  LRESULT OnDeleteItem(NMHDR* header);
  LRESULT OnCustomDraw(NMHDR* header);
  LRESULT OnDpiChanged(UINT message, WPARAM w_param, LPARAM l_param,
                       BOOL& handled);

  void OnFileUpdate(UINT notify_code, int id, CWindow control);
  void OnEditSelectAll(UINT notify_code, int id, CWindow control);
  void OnFileMoveToTemp(UINT notify_code, int id, CWindow control);
  void OnEditDelete(UINT notify_code, int id, CWindow control);
  void ApplySort();
  void RebuildFonts();
  void DisposeFonts();
  void SyncListAppearance();
  void LayoutChildren();
  void UpdateListColumns();
  void RefreshChrome(bool pulse_selection = false);
  void BeginSettleAnimation();
  void EnsureAnimationTimer();
  int GetButtonAtPoint(CPoint point) const;
  CRect GetButtonRect(int command_id) const;
  bool IsButtonEnabled(int command_id) const;
  void UpdateHotButton(int command_id);
  void InvokeSurfaceButton(int command_id);
  void DrawSurfaceButton(CDCHandle dc, const CRect& rect, int command_id) const;
  void PaintChrome(CDCHandle dc);
  void PaintCommandBand(CDCHandle dc, const CRect& rect) const;
  void PaintListFrame(CDCHandle dc, const CRect& rect) const;
  void PaintActionRail(CDCHandle dc, const CRect& rect) const;
  LRESULT PaintHeaderCustomDraw(NMCUSTOMDRAW* draw);
  LRESULT PaintListCustomDraw(NMLVCUSTOMDRAW* draw);
  CString BuildScanStateLine() const;
  CString BuildScanDetailLine() const;
  CString BuildSelectionStateLine() const;
  CString BuildSelectionDetailLine() const;
  static int CALLBACK CompareFileItems(LPARAM left, LPARAM right,
                                       LPARAM context);

  static constexpr UINT kAnimationTimerId = 1;

  UINT dpi_;
  uint64_t selected_size_;
  uint64_t moved_size_;
  uint64_t deleted_size_;
  uint64_t total_reclaimable_size_;
  int selected_count_;
  int total_reclaimable_count_;
  int sort_column_;
  int hot_button_;
  int pressed_button_;
  int reveal_progress_;
  int action_progress_;
  int selected_flash_;
  int moved_flash_;
  int deleted_flash_;
  int scan_flash_;
  bool sort_ascending_;
  bool tracking_mouse_;
  bool has_last_scan_;
  bool last_scan_succeeded_;
  SYSTEMTIME last_scan_time_;
  CRect command_band_rect_;
  CRect list_frame_rect_;
  CRect action_rail_rect_;
  CRect scan_button_rect_;
  CRect select_all_button_rect_;
  CRect move_to_temp_button_rect_;
  CRect delete_button_rect_;
  CFont headline_font_;
  CFont title_font_;
  CFont body_font_;
  CFont caption_font_;
  CFont button_font_;
  CListViewCtrl file_list_;

  MainFrame(const MainFrame&) = delete;
  MainFrame& operator=(const MainFrame&) = delete;
};

}  // namespace ui
}  // namespace patch_cleaner

#endif  // PATCH_CLEANER_UI_MAIN_FRAME_H_
