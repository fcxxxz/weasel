#include "stdafx.h"
#include "InstallOptionsDlg.h"
#include <atlstr.h>
#include <ShlObj.h>
#include <ShellScalingApi.h>
#include <WeaselUtility.h>
#pragma comment(lib, "Shell32.lib")

int uninstall(bool silent);

InstallOptionsDialog::InstallOptionsDialog()
    : installed(false), profile(L"hans"), user_dir() {}

InstallOptionsDialog::~InstallOptionsDialog() {
  m_controlOriginalRects.clear();
  if (m_currentFont) {
    DeleteObject(m_currentFont);
  }
  m_currentFont = nullptr;
}

UINT InstallOptionsDialog::GetWindowDpi() {
  HMONITOR hMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
    if (dpiX == 0)
      dpiX = 96;
    return dpiX;
  }
  return 96;
}

LRESULT InstallOptionsDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
  profile_.Attach(GetDlgItem(IDC_COMBO_PROFILE));
  remove_.Attach(GetDlgItem(IDC_REMOVE));
  default_dir_.Attach(GetDlgItem(IDC_RADIO_DEFAULT_DIR));
  custom_dir_.Attach(GetDlgItem(IDC_RADIO_CUSTOM_DIR));
  dir_.Attach(GetDlgItem(IDC_EDIT_DIR));

  struct ProfileOption {
    const wchar_t* value;
    const wchar_t* zhHansText;
    const wchar_t* zhHantText;
    const wchar_t* enText;
  };
  const ProfileOption options[] = {
      {L"hans", L"中文（简体，中国大陆）", L"中文（簡體，中國大陸）",
       L"Chinese (Simplified, China)"},
      {L"hant", L"中文（繁体，中国台湾）", L"中文（繁體，中國臺灣）",
       L"Chinese (Traditional, Taiwan)"},
      {L"hongkong", L"中文（繁体，中国香港）", L"中文（繁體，中國香港）",
       L"Chinese (Traditional, Hong Kong)"},
      {L"macau", L"中文（繁体，中国澳门）", L"中文（繁體，中國澳門）",
       L"Chinese (Traditional, Macao)"},
      {L"singapore", L"中文（简体，新加坡）", L"中文（簡體，新加坡）",
       L"Chinese (Simplified, Singapore)"},
  };
  const auto uiLang = PRIMARYLANGID(GetThreadUILanguage());
  const auto uiSubLang = SUBLANGID(GetThreadUILanguage());

  int selected = 0;
  for (int i = 0; i < static_cast<int>(_countof(options)); ++i) {
    const wchar_t* label = options[i].enText;
    if (uiLang == LANG_CHINESE) {
      const bool isTraditional = (uiSubLang == SUBLANG_CHINESE_TRADITIONAL ||
                                  uiSubLang == SUBLANG_CHINESE_HONGKONG ||
                                  uiSubLang == SUBLANG_CHINESE_MACAU);
      label = isTraditional ? options[i].zhHantText : options[i].zhHansText;
    }
    int index = profile_.AddString(label);
    profile_.SetItemData(index, i);
    if (profile == options[i].value) {
      selected = index;
    }
  }
  profile_.SetCurSel(selected);

  CheckRadioButton(
      IDC_RADIO_DEFAULT_DIR, IDC_RADIO_CUSTOM_DIR,
      (user_dir.empty() ? IDC_RADIO_DEFAULT_DIR : IDC_RADIO_CUSTOM_DIR));
  dir_.SetWindowTextW(user_dir.c_str());

  profile_.EnableWindow(!installed);
  remove_.EnableWindow(installed);
  dir_.EnableWindow(user_dir.empty() ? FALSE : TRUE);

  button_custom_dir_.Attach(GetDlgItem(IDC_BUTTON_CUSTOM_DIR));
  button_custom_dir_.EnableWindow(user_dir.empty() ? FALSE : TRUE);

  ok_.Attach(GetDlgItem(IDOK));
  if (installed) {
    CString str;
    str.LoadStringW(IDS_STRING_MODIFY);
    ok_.SetWindowTextW(str);
  }

  // 保存所有控件的初始位置和字体
  ::GetWindowRect(m_hWnd, &m_rect);
  EnumChildWindows(
      m_hWnd,
      [](HWND hWnd, LPARAM lParam) {
        auto pThis = reinterpret_cast<InstallOptionsDialog*>(lParam);
        // 保存控件位置
        RECT rect, rcDlg;
        ::GetClientRect(pThis->m_hWnd, &rcDlg);
        ::ClientToScreen(pThis->m_hWnd, (LPPOINT)&rcDlg.left);
        ::ClientToScreen(pThis->m_hWnd, (LPPOINT)&rcDlg.right);
        ::GetWindowRect(hWnd, &rect);
        rect.left -= rcDlg.left;
        rect.right -= rcDlg.left;
        rect.top -= rcDlg.top;
        rect.bottom -= rcDlg.top;
        pThis->m_controlOriginalRects[hWnd] = rect;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(this));

  UINT newDpi = GetWindowDpi();
  if (newDpi != 96) {
    float scaleFactor = (float)newDpi / (float)96.0f;
    GetWindowRect(&m_rect);
    int width = (m_rect.right - m_rect.left) * scaleFactor;
    int height = (m_rect.bottom - m_rect.top) * scaleFactor;
    // 调整窗口大小
    SetWindowPos(nullptr, m_rect.left, m_rect.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    ScaleControlsAndFonts(newDpi);
    Invalidate();
  }
  m_currentDpi = newDpi;
  CenterWindow();
  return 0;
}

LRESULT InstallOptionsDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}

void InstallOptionsDialog::ScaleControlsAndFonts(UINT newDpi) {
  const float scaleFactor = static_cast<float>(newDpi) / 96;
  // 缩放控件位置和大小
  for (const auto& [hWnd, originalRect] : m_controlOriginalRects) {
    int newX = static_cast<int>(originalRect.left * scaleFactor);
    int newY = static_cast<int>(originalRect.top * scaleFactor);
    int newWidth =
        static_cast<int>(originalRect.right - originalRect.left) * scaleFactor;
    int newHeight =
        static_cast<int>(originalRect.bottom - originalRect.top) * scaleFactor;
    ::SetWindowPos(hWnd, nullptr, newX, newY, newWidth, newHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }
  // 缩放字体
  HFONT oldFont = (HFONT)::SendMessage(m_hWnd, WM_GETFONT, 0, 0);
  LOGFONT lf;
  if (!GetObject(oldFont, sizeof(lf), &lf)) {
    return;
  }
  lf.lfHeight = static_cast<int>(lf.lfHeight * scaleFactor);
  HFONT hNewFont = CreateFontIndirect(&lf);
  if (m_currentFont) {
    DeleteObject(m_currentFont);
  }
  ::SendMessage(m_hWnd, WM_SETFONT, (WPARAM)hNewFont, true);
  for (const auto& [hWnd, rect] : m_controlOriginalRects) {
    ::SendMessage(hWnd, WM_SETFONT, (WPARAM)hNewFont, true);
  }
  m_currentFont = hNewFont;
}

LRESULT InstallOptionsDialog::OnDpiChanged(UINT,
                                           WPARAM wParam,
                                           LPARAM lParam,
                                           BOOL&) {
  UINT newDpi = HIWORD(wParam);
  if (newDpi == m_currentDpi)
    return 0;
  float scaleFactor = (float)newDpi / (float)96.0f;
  const RECT* pNewRect = reinterpret_cast<const RECT*>(lParam);
  int width = (m_rect.right - m_rect.left) * scaleFactor;
  int height = (m_rect.bottom - m_rect.top) * scaleFactor;
  // 调整窗口大小
  SetWindowPos(nullptr, pNewRect->left, pNewRect->top, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
  ScaleControlsAndFonts(newDpi);
  m_currentDpi = newDpi;
  return 0;
}

LRESULT InstallOptionsDialog::OnOK(WORD, WORD code, HWND, BOOL&) {
  int selected = profile_.GetCurSel();
  if (selected == CB_ERR) {
    selected = 0;
  }
  switch (static_cast<int>(profile_.GetItemData(selected))) {
    case 1:
      profile = L"hant";
      break;
    case 2:
      profile = L"hongkong";
      break;
    case 3:
      profile = L"macau";
      break;
    case 4:
      profile = L"singapore";
      break;
    default:
      profile = L"hans";
      break;
  }
  if (IsDlgButtonChecked(IDC_RADIO_CUSTOM_DIR) == BST_CHECKED) {
    CStringW text;
    dir_.GetWindowTextW(text);
    user_dir = text;
  } else {
    user_dir.clear();
  }
  EndDialog(IDOK);
  return 0;
}

LRESULT InstallOptionsDialog::OnRemove(WORD, WORD code, HWND, BOOL&) {
  const bool non_silent = false;
  uninstall(non_silent);
  installed = false;
  CString str;
  str.LoadStringW(IDS_STRING_INSTALL);
  ok_.SetWindowTextW(str);
  profile_.EnableWindow(!installed);
  remove_.EnableWindow(installed);
  return 0;
}

LRESULT InstallOptionsDialog::OnUseDefaultDir(WORD, WORD code, HWND, BOOL&) {
  dir_.EnableWindow(FALSE);
  dir_.SetWindowTextW(L"");
  button_custom_dir_.EnableWindow(FALSE);
  return 0;
}

LRESULT InstallOptionsDialog::OnUseCustomDir(WORD, WORD code, HWND, BOOL&) {
  CShellFileOpenDialog fileOpenDlg(
      NULL, FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS);
  CStringW text;
  dir_.GetWindowTextW(text);
  if (!text.IsEmpty()) {
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr = SHParseDisplayName(text, NULL, &pidl, 0, NULL);
    if (SUCCEEDED(hr)) {
      IShellItem* psi;
      hr = SHCreateShellItem(NULL, NULL, pidl, &psi);
      if (SUCCEEDED(hr)) {
        fileOpenDlg.GetPtr()->SetFolder(psi);
        psi->Release();
      }
      CoTaskMemFree(pidl);
    }
  }
  if (fileOpenDlg.DoModal(m_hWnd) == IDOK) {
    CComPtr<IShellItem> psi;
    if (SUCCEEDED(fileOpenDlg.GetPtr()->GetResult(&psi))) {
      LPWSTR path;
      if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        dir_.SetWindowTextW(path);
        CoTaskMemFree(path);
      }
    }
  }
  dir_.EnableWindow(TRUE);
  button_custom_dir_.EnableWindow(TRUE);
  ok_.SetFocus();
  return 0;
}
