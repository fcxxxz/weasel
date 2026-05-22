#include "stdafx.h"
#include <resource.h>
#include <thread>
#include <shellapi.h>
#include "WeaselTSF.h"
#include "LanguageBar.h"
#include "CandidateList.h"
#include <WeaselUtility.h>

static const DWORD LANGBARITEMSINK_COOKIE = 0x42424242;

static std::wstring HandleValue(const void* handle) {
  return std::to_wstring(reinterpret_cast<uintptr_t>(handle));
}

static void HMENU2ITfMenu(HMENU hMenu, ITfMenu* pTfMenu) {
  /* NOTE: Only limited functions are supported */
  int N = GetMenuItemCount(hMenu);
  for (int i = 0; i < N; i++) {
    MENUITEMINFO mii;
    mii.cbSize = sizeof(MENUITEMINFO);
    mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING;
    mii.dwTypeData = NULL;
    if (GetMenuItemInfo(hMenu, i, TRUE, &mii)) {
      UINT id = mii.wID;
      if (mii.fType == MFT_SEPARATOR)
        pTfMenu->AddMenuItem(id, TF_LBMENUF_SEPARATOR, NULL, NULL, NULL, 0,
                             NULL);
      else if (mii.fType == MFT_STRING) {
        mii.dwTypeData = (LPWSTR)malloc(sizeof(WCHAR) * (mii.cch + 1));
        mii.cch++;
        if (GetMenuItemInfo(hMenu, i, TRUE, &mii))
          pTfMenu->AddMenuItem(id, 0, NULL, NULL, mii.dwTypeData, mii.cch,
                               NULL);
        free(mii.dwTypeData);
      }
    }
  }
}

static bool QueryMachineStringValue(LPCWSTR key_path,
                                    LPCWSTR value_name,
                                    REGSAM view,
                                    std::wstring& value) {
  HKEY key = NULL;
  LSTATUS status =
      RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, KEY_READ | view, &key);
  if (status != ERROR_SUCCESS)
    return false;

  WCHAR buffer[MAX_PATH] = {0};
  DWORD type = 0;
  DWORD bytes = sizeof(buffer);
  status = RegQueryValueExW(key, value_name, NULL, &type,
                            reinterpret_cast<LPBYTE>(buffer), &bytes);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
    return false;

  value = buffer;
  if (type == REG_EXPAND_SZ) {
    WCHAR expanded[MAX_PATH] = {0};
    if (ExpandEnvironmentStringsW(value.c_str(), expanded, _countof(expanded)))
      value = expanded;
  }
  return !value.empty();
}

static bool QueryMachineStringValueAllViews(LPCWSTR key_path,
                                            LPCWSTR value_name,
                                            std::wstring& value) {
  if (is_wow64() &&
      QueryMachineStringValue(key_path, value_name, KEY_WOW64_64KEY, value))
    return true;
  if (QueryMachineStringValue(key_path, value_name, 0, value))
    return true;
  return is_wow64() &&
         QueryMachineStringValue(key_path, value_name, KEY_WOW64_32KEY, value);
}

static bool QueryWeaselRoot(std::wstring& dir) {
  bool ok = QueryMachineStringValueAllViews(L"Software\\Rime\\Weasel",
                                            L"WeaselRoot", dir);
  WeaselDebugLog(L"LanguageBar",
                 L"QueryWeaselRoot ok=" + std::to_wstring(ok) + L" dir=" +
                     dir);
  return ok;
}

static bool QueryWeaselRootFromRunKey(std::wstring& dir) {
  std::wstring command;
  if (!QueryMachineStringValueAllViews(
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"WeaselServer",
          command)) {
    WeaselDebugLog(L"LanguageBar", L"Run key WeaselServer not found");
    return false;
  }
  bool ok = weasel::ServiceRootFromCommandLine(command, dir);
  WeaselDebugLog(L"LanguageBar",
                 L"Run key command=" + command + L" parse_ok=" +
                     std::to_wstring(ok) + L" dir=" + dir);
  return ok;
}

static bool QueryWeaselRootFromModule(std::wstring& dir) {
  dir.clear();
  WCHAR path[MAX_PATH] = {0};
  DWORD length = GetModuleFileNameW(g_hInst, path, _countof(path));
  if (length == 0 || length >= _countof(path)) {
    WeaselDebugLog(L"LanguageBar", L"Module root lookup failed");
    return false;
  }

  std::wstring module_path = path;
  size_t slash = module_path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    WeaselDebugLog(L"LanguageBar",
                   L"Module root lookup failed: no directory");
    return false;
  }

  std::wstring module_dir = module_path.substr(0, slash);
  std::wstring server = weasel::ServiceExecutablePath(module_dir);
  bool ok = !server.empty() && fs::exists(server);
  WeaselDebugLog(L"LanguageBar",
                 L"Module root lookup module=" + module_path + L" ok=" +
                     std::to_wstring(ok) + L" dir=" + module_dir +
                     L" server=" + server);
  if (!ok)
    return false;

  dir = module_dir;
  return true;
}

static void ShowStartServiceFailure(const std::wstring& server) {
  std::wstring message = L"无法启动算法服务";
  if (!server.empty())
    message += L":\n" + server;
  MessageBoxW(NULL, message.c_str(), get_weasel_ime_name().c_str(),
              MB_ICONERROR | MB_OK);
}

static void ShowServiceQuitFailure() {
  MessageBoxW(NULL, L"无法退出算法服务", get_weasel_ime_name().c_str(),
              MB_ICONERROR | MB_OK);
}

static bool StartWeaselServer(const std::wstring& dir, bool restart) {
  std::wstring server = weasel::ServiceExecutablePath(dir);
  WeaselDebugLog(L"LanguageBar",
                 L"StartWeaselServer restart=" + std::to_wstring(restart) +
                     L" dir=" + dir + L" server=" + server);
  if (server.empty()) {
    WeaselDebugLog(L"LanguageBar", L"StartWeaselServer failed: empty server");
    ShowStartServiceFailure(server);
    return false;
  }
  SetLastError(ERROR_SUCCESS);
  HINSTANCE result = ShellExecuteW(
      NULL, L"open", server.c_str(),
      restart ? weasel::ServiceManualRestartArgument() : NULL, dir.c_str(),
      SW_HIDE);
  if ((uintptr_t)result <= 32) {
    WeaselDebugLog(L"LanguageBar",
                   L"ShellExecute failed result=" +
                       std::to_wstring((uintptr_t)result) + L" last_error=" +
                       std::to_wstring(GetLastError()));
    ShowStartServiceFailure(server);
    return false;
  }
  WeaselDebugLog(L"LanguageBar",
                 L"ShellExecute ok result=" +
                     std::to_wstring((uintptr_t)result));
  return true;
}

static bool open(const std::wstring& path) {
  std::wstring quoted_path = L"\"" + path + L"\"";
  return (uintptr_t)ShellExecuteW(NULL, L"open", quoted_path.c_str(), NULL,
                                  NULL, SW_SHOWNORMAL) > 32;
}

CLangBarItemButton::CLangBarItemButton(com_ptr<WeaselTSF> pTextService,
                                       REFGUID guid,
                                       weasel::UIStyle& style)
    : _status(0),
      _style(style),
      _current_schema_zhung_icon(),
      _current_schema_ascii_icon() {
  DllAddRef();

  _pLangBarItemSink = NULL;
  _cRef = 1;
  _pTextService = pTextService;
  _guid = guid;
  ascii_mode = false;
  WeaselDebugLog(L"LanguageBar",
                 L"CLangBarItemButton created this=" + HandleValue(this));
}

CLangBarItemButton::~CLangBarItemButton() {
  WeaselDebugLog(L"LanguageBar",
                 L"CLangBarItemButton destroyed this=" + HandleValue(this));
  DllRelease();
}

STDAPI CLangBarItemButton::QueryInterface(REFIID riid, void** ppvObject) {
  if (ppvObject == NULL)
    return E_INVALIDARG;

  *ppvObject = NULL;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem) ||
      IsEqualIID(riid, IID_ITfLangBarItemButton))
    *ppvObject = (ITfLangBarItemButton*)this;
  else if (IsEqualIID(riid, IID_ITfSource))
    *ppvObject = (ITfSource*)this;

  if (*ppvObject) {
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

STDAPI_(ULONG) CLangBarItemButton::AddRef() {
  return ++_cRef;
}

STDAPI_(ULONG) CLangBarItemButton::Release() {
  LONG cr = --_cRef;
  assert(_cRef >= 0);
  if (_cRef == 0)
    delete this;
  return cr;
}

STDAPI CLangBarItemButton::GetInfo(TF_LANGBARITEMINFO* pInfo) {
  pInfo->clsidService = c_clsidTextService;
  pInfo->guidItem = _guid;
  pInfo->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_MENU |
                   TF_LBI_STYLE_SHOWNINTRAY;
  pInfo->ulSort = 1;
  lstrcpyW(pInfo->szDescription, L"WeaselTSF Button");
  return S_OK;
}

STDAPI CLangBarItemButton::GetStatus(DWORD* pdwStatus) {
  *pdwStatus = _status;
  return S_OK;
}

STDAPI CLangBarItemButton::Show(BOOL fShow) {
  SetLangbarStatus(TF_LBI_STATUS_HIDDEN, fShow ? FALSE : TRUE);
  return S_OK;
}

static LANGID GetActiveProfileLangId() {
  CComPtr<ITfInputProcessorProfileMgr> pInputProcessorProfileMgr;
  HRESULT hr = pInputProcessorProfileMgr.CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_ALL);
  if (FAILED(hr))
    return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

  TF_INPUTPROCESSORPROFILE profile;
  hr = pInputProcessorProfileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD,
                                                   &profile);
  if (FAILED(hr))
    return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
  return profile.langid;
}

STDAPI CLangBarItemButton::GetTooltipString(BSTR* pbstrToolTip) {
  LANGID langid = get_language_id();
  if (langid == TEXTSERVICE_LANGID_HANS) {
    *pbstrToolTip = SysAllocString(L"左键切换模式，右键打开菜单");
  } else if (langid == TEXTSERVICE_LANGID_HANT) {
    *pbstrToolTip = SysAllocString(L"左鍵切換模式，右鍵打開菜單");
  } else {
    *pbstrToolTip = SysAllocString(
        L"Left-click to switch modes\n\nRight-click for more options");
  }

  return (*pbstrToolTip == NULL) ? E_OUTOFMEMORY : S_OK;
}

STDAPI CLangBarItemButton::OnClick(TfLBIClick click,
                                   POINT pt,
                                   const RECT* prcArea) {
  WeaselDebugLog(L"LanguageBar",
                 L"OnClick click=" + std::to_wstring(click) + L" pt=(" +
                     std::to_wstring(pt.x) + L"," + std::to_wstring(pt.y) +
                     L") area_null=" + std::to_wstring(prcArea == NULL));
  if (click == TF_LBI_CLK_LEFT) {
    UINT wID =
        ascii_mode ? ID_WEASELTRAY_DISABLE_ASCII : ID_WEASELTRAY_ENABLE_ASCII;
    WeaselDebugLog(L"LanguageBar",
                   L"OnClick left dispatch wID=" + std::to_wstring(wID));
    _pTextService->_HandleLangBarMenuSelect(wID);
    ascii_mode = !ascii_mode;
    if (_pLangBarItemSink) {
      _pLangBarItemSink->OnUpdate(TF_LBI_STATUS | TF_LBI_ICON);
    }
  } else if (click == TF_LBI_CLK_RIGHT) {
    /* Open menu */
    HWND hwnd = _pTextService->_GetFocusedContextWindow();
    WeaselDebugLog(L"LanguageBar",
                   L"OnClick right focused_hwnd=" + HandleValue(hwnd));
    if (hwnd != NULL) {
      LANGID langid = get_language_id();
      UINT menu_resource = IDR_MENU_POPUP;
      if (langid == TEXTSERVICE_LANGID_HANS) {
        menu_resource = IDR_MENU_POPUP_HANS;
      } else if (langid == TEXTSERVICE_LANGID_HANT) {
        menu_resource = IDR_MENU_POPUP_HANT;
      }
      SetLastError(ERROR_SUCCESS);
      HMENU menu = LoadMenuW(g_hInst, MAKEINTRESOURCE(menu_resource));
      WeaselDebugLog(L"LanguageBar",
                     L"OnClick right LoadMenu resource=" +
                         std::to_wstring(menu_resource) + L" menu=" +
                         HandleValue(menu) + L" last_error=" +
                         std::to_wstring(GetLastError()));
      if (menu == NULL)
        return S_OK;
      HMENU popupMenu = GetSubMenu(menu, 0);
      WeaselDebugLog(L"LanguageBar",
                     L"OnClick right popup=" + HandleValue(popupMenu));
      if (popupMenu == NULL) {
        DestroyMenu(menu);
        return S_OK;
      }
      UINT wID = TrackPopupMenuEx(
          popupMenu, TPM_NONOTIFY | TPM_RETURNCMD | TPM_HORPOSANIMATION, pt.x,
          pt.y, hwnd, NULL);
      WeaselDebugLog(L"LanguageBar",
                     L"OnClick right TrackPopupMenuEx wID=" +
                         std::to_wstring(wID));
      DestroyMenu(menu);
      if (weasel::IsTrayMenuSelectionCancelled(wID)) {
        WeaselDebugLog(L"LanguageBar",
                       L"OnClick right menu cancelled or dismissed");
        return S_OK;
      }
      _pTextService->_HandleLangBarMenuSelect(wID);
    } else {
      WeaselDebugLog(L"LanguageBar",
                     L"OnClick right ignored: no focused context window");
    }
  }
  return S_OK;
}

STDAPI CLangBarItemButton::InitMenu(ITfMenu* pMenu) {
  WeaselDebugLog(L"LanguageBar",
                 L"InitMenu pMenu=" + HandleValue(pMenu));
  SetLastError(ERROR_SUCCESS);
  HMENU menu = LoadMenuW(g_hInst, MAKEINTRESOURCE(IDR_MENU_POPUP));
  WeaselDebugLog(L"LanguageBar",
                 L"InitMenu LoadMenu menu=" + HandleValue(menu) +
                     L" last_error=" + std::to_wstring(GetLastError()));
  if (menu == NULL)
    return E_FAIL;
  HMENU popupMenu = GetSubMenu(menu, 0);
  WeaselDebugLog(L"LanguageBar",
                 L"InitMenu popup=" + HandleValue(popupMenu));
  if (popupMenu == NULL) {
    DestroyMenu(menu);
    return E_FAIL;
  }
  HMENU2ITfMenu(popupMenu, pMenu);
  DestroyMenu(menu);
  return S_OK;
}

STDAPI CLangBarItemButton::OnMenuSelect(UINT wID) {
  WeaselDebugLog(L"LanguageBar",
                 L"OnMenuSelect wID=" + std::to_wstring(wID));
  _pTextService->_HandleLangBarMenuSelect(wID);
  return S_OK;
}

STDAPI CLangBarItemButton::GetIcon(HICON* phIcon) {
  if (ascii_mode) {
    if (_style.current_ascii_icon.empty())
      *phIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_EN), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    else
      *phIcon =
          (HICON)LoadImageW(NULL, _style.current_ascii_icon.c_str(), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON),
                            GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
  } else {
    if (_style.current_zhung_icon.empty())
      *phIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_ZH), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    else
      *phIcon =
          (HICON)LoadImageW(NULL, _style.current_zhung_icon.c_str(), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON),
                            GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
  }
  return (*phIcon == NULL) ? E_FAIL : S_OK;
}

STDAPI CLangBarItemButton::GetText(BSTR* pbstrText) {
  *pbstrText = SysAllocString(L"WeaselTSF Button");
  return (*pbstrText == NULL) ? E_OUTOFMEMORY : S_OK;
}

STDAPI CLangBarItemButton::AdviseSink(REFIID riid,
                                      IUnknown* punk,
                                      DWORD* pdwCookie) {
  if (!IsEqualIID(riid, IID_ITfLangBarItemSink))
    return CONNECT_E_CANNOTCONNECT;
  if (_pLangBarItemSink != NULL)
    return CONNECT_E_ADVISELIMIT;

  if (punk->QueryInterface(IID_ITfLangBarItemSink,
                           (LPVOID*)&_pLangBarItemSink) != S_OK) {
    _pLangBarItemSink = NULL;
    return E_NOINTERFACE;
  }
  *pdwCookie = LANGBARITEMSINK_COOKIE;
  return S_OK;
}

STDAPI CLangBarItemButton::UnadviseSink(DWORD dwCookie) {
  if (dwCookie != LANGBARITEMSINK_COOKIE || _pLangBarItemSink == NULL)
    return CONNECT_E_NOCONNECTION;
  _pLangBarItemSink = NULL;
  return S_OK;
}

void CLangBarItemButton::UpdateWeaselStatus(weasel::Status stat) {
  if (stat.ascii_mode != ascii_mode) {
    ascii_mode = stat.ascii_mode;
  }
  if (_current_schema_zhung_icon != _style.current_zhung_icon) {
    _current_schema_zhung_icon = _style.current_zhung_icon;
  }
  if (_current_schema_ascii_icon != _style.current_ascii_icon) {
    _current_schema_ascii_icon = _style.current_ascii_icon;
  }
  if (_pLangBarItemSink) {
    _pLangBarItemSink->OnUpdate(TF_LBI_STATUS | TF_LBI_ICON);
  }
}

void CLangBarItemButton::SetLangbarStatus(DWORD dwStatus, BOOL fSet) {
  BOOL isChange = FALSE;

  if (fSet) {
    if (!(_status & dwStatus)) {
      _status |= dwStatus;
      isChange = TRUE;
    }
  } else {
    if (_status & dwStatus) {
      _status &= ~dwStatus;
      isChange = TRUE;
    }
  }

  if (isChange && _pLangBarItemSink) {
    _pLangBarItemSink->OnUpdate(TF_LBI_STATUS | TF_LBI_ICON);
  }

  return;
}

std::wstring WeaselTSF::_GetRootDir() {
  std::wstring dir{};
  if (QueryWeaselRoot(dir))
    return dir;
  if (QueryWeaselRootFromRunKey(dir))
    return dir;
  QueryWeaselRootFromModule(dir);
  return dir;
}

void WeaselTSF::_HandleLangBarMenuSelect(UINT wID) {
  WeaselDebugLog(L"LanguageBar",
                 L"HandleLangBarMenuSelect wID=" + std::to_wstring(wID));
  if (weasel::IsTrayMenuSelectionCancelled(wID)) {
    WeaselDebugLog(L"LanguageBar",
                   L"HandleLangBarMenuSelect ignored cancelled menu");
    return;
  }
  std::wstring dir{};
  switch (wID) {
    case ID_WEASELTRAY_RERUN_SERVICE:
    case ID_WEASELTRAY_INSTALLDIR:
      dir = _GetRootDir();
      if (wID == ID_WEASELTRAY_RERUN_SERVICE) {
        WeaselDebugLog(L"LanguageBar",
                       L"Handle rerun service menu root_dir=" + dir);
      }
      if (!dir.empty()) {
        if (wID == ID_WEASELTRAY_RERUN_SERVICE) {
          std::thread th([dir]() {
            StartWeaselServer(dir, true);
          });
          th.detach();
        } else
          open(dir);
      } else if (wID == ID_WEASELTRAY_RERUN_SERVICE) {
        WeaselDebugLog(L"LanguageBar",
                       L"Handle rerun service menu failed: empty root_dir");
        ShowStartServiceFailure(L"");
      }
      break;
    case ID_WEASELTRAY_USERCONFIG:
      if (FAILED(RegGetStringValue(HKEY_CURRENT_USER, L"Software\\Rime\\Weasel",
                                   L"RimeUserDir", dir)) ||
          dir.empty()) {
        WCHAR _path[MAX_PATH] = {0};
        ExpandEnvironmentStringsW(L"%AppData%\\Rime", _path, _countof(_path));
        dir = std::wstring(_path);
      }
      if (!dir.empty() && fs::exists(dir))
        open(dir);
      else
        MessageBoxW(NULL, (L"Not found: " + dir).c_str(), L"RimeUserDir",
                    MB_ICONERROR | MB_OK);
      break;
    case ID_WEASELTRAY_LOGDIR:
      open(WeaselLogPath().wstring());
      break;
    case ID_WEASELTRAY_QUIT:
      if (!m_client.TrayCommandSync(wID))
        ShowServiceQuitFailure();
      break;
    case ID_WEASELTRAY_WIKI:
      open(L"https://rime.im/docs/");
      break;
    case ID_WEASELTRAY_FORUM:
      open(L"https://rime.im/discuss/");
      break;
    default:
      WeaselDebugLog(L"LanguageBar",
                     L"Forward TrayCommand wID=" + std::to_wstring(wID));
      m_client.TrayCommand(wID);
      break;
  }
}

HWND WeaselTSF::_GetFocusedContextWindow() {
  HWND hwnd = NULL;
  ITfDocumentMgr* pDocMgr;
  if (_pThreadMgr->GetFocus(&pDocMgr) == S_OK && pDocMgr != NULL) {
    ITfContext* pContext;
    if (pDocMgr->GetTop(&pContext) == S_OK && pContext != NULL) {
      ITfContextView* pContextView;
      if (pContext->GetActiveView(&pContextView) == S_OK &&
          pContextView != NULL) {
        pContextView->GetWnd(&hwnd);
        pContextView->Release();
      }
      pContext->Release();
    }
    pDocMgr->Release();
  }

  if (hwnd == NULL) {
    HWND hwndForeground = GetForegroundWindow();
    if (GetWindowThreadProcessId(hwndForeground, NULL) == GetCurrentThreadId())
      hwnd = hwndForeground;
  }

  return hwnd;
}

BOOL WeaselTSF::_InitLanguageBar() {
  com_ptr<ITfLangBarItemMgr> pLangBarItemMgr;
  BOOL fRet = FALSE;

  WeaselDebugLog(L"LanguageBar", L"InitLanguageBar start");
  HRESULT hr = _pThreadMgr->QueryInterface(&pLangBarItemMgr);
  WeaselDebugLog(L"LanguageBar",
                 L"InitLanguageBar QueryInterface hr=" +
                     std::to_wstring(hr));
  if (hr != S_OK)
    return FALSE;

  if ((_pLangBarButton = new CLangBarItemButton(this, GUID_LBI_INPUTMODE,
                                                _cand->style())) == NULL)
    return FALSE;

  hr = pLangBarItemMgr->AddItem(_pLangBarButton);
  WeaselDebugLog(L"LanguageBar",
                 L"InitLanguageBar AddItem hr=" + std::to_wstring(hr));
  if (hr != S_OK) {
    _pLangBarButton = NULL;
    return FALSE;
  }

  _pLangBarButton->Show(TRUE);
  WeaselDebugLog(L"LanguageBar", L"InitLanguageBar Show(TRUE)");
  fRet = TRUE;

  return fRet;
}

void WeaselTSF::_UninitLanguageBar() {
  com_ptr<ITfLangBarItemMgr> pLangBarItemMgr;

  if (_pLangBarButton == NULL) {
    WeaselDebugLog(L"LanguageBar",
                   L"UninitLanguageBar skipped: no langbar button");
    return;
  }

  if (_pThreadMgr->QueryInterface(&pLangBarItemMgr) == S_OK) {
    WeaselDebugLog(L"LanguageBar", L"UninitLanguageBar RemoveItem");
    pLangBarItemMgr->RemoveItem(_pLangBarButton);
  }

  _pLangBarButton = NULL;
  WeaselDebugLog(L"LanguageBar", L"UninitLanguageBar done");
}

void WeaselTSF::_UpdateLanguageBar(weasel::Status stat) {
  if (!_pLangBarButton)
    return;
  DWORD flags;
  _GetCompartmentDWORD(flags, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
  if (stat.ascii_mode)
    flags &= (~TF_CONVERSIONMODE_NATIVE);
  else
    flags |= TF_CONVERSIONMODE_NATIVE;
  if (stat.full_shape)
    flags |= TF_CONVERSIONMODE_FULLSHAPE;
  else
    flags &= (~TF_CONVERSIONMODE_FULLSHAPE);
  _SetCompartmentDWORD(flags, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);

  _pLangBarButton->UpdateWeaselStatus(stat);
}

void WeaselTSF::_ShowLanguageBar(BOOL show) {
  if (!_pLangBarButton)
    return;
  _pLangBarButton->Show(show);
}

void WeaselTSF::_EnableLanguageBar(BOOL enable) {
  if (!_pLangBarButton)
    return;
  _pLangBarButton->SetLangbarStatus(TF_LBI_STATUS_DISABLED, !enable);
}
