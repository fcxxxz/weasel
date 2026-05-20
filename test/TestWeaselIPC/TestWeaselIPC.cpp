// TestWeaselIPC.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <WeaselIPC.h>
#include <RimeWithWeasel.h>
#include <resource.h>
#include "../../WeaselIPCServer/WeaselServerImpl.h"

#include <boost/detail/lightweight_test.hpp>
#include <boost/interprocess/streams/bufferstream.hpp>
using namespace boost::interprocess;

#include <iostream>
#include <memory>

CAppModule _Module;

int console_main();
int client_main();
int server_main();
int unit_main();

// usage: TestWeaselIPC.exe [/start | /stop | /console]

int _tmain(int argc, _TCHAR* argv[]) {
  if (argc == 1)  // no args
  {
    return client_main();
  } else if (argc > 1 && !wcscmp(L"/start", argv[1])) {
    return server_main();
  } else if (argc > 1 && !wcscmp(L"/stop", argv[1])) {
    weasel::Client client;
    if (!client.Connect()) {
      std::cerr << "server not running." << std::endl;
      return 0;
    }
    client.ShutdownServer();
    return 0;
  } else if (argc > 1 && !wcscmp(L"/console", argv[1])) {
    return console_main();
    return 0;
  } else if (argc > 1 && !wcscmp(L"/unit", argv[1])) {
    return unit_main();
  }

  return -1;
}

int unit_main() {
  BOOST_TEST(!weasel::IsKeyEventResultProcessed(0));
  BOOST_TEST(!weasel::IsKeyEventResultEaten(0));

  DWORD not_eaten = weasel::MakeKeyEventResult(FALSE);
  BOOST_TEST(weasel::IsKeyEventResultProcessed(not_eaten));
  BOOST_TEST(!weasel::IsKeyEventResultEaten(not_eaten));

  DWORD eaten = weasel::MakeKeyEventResult(TRUE);
  BOOST_TEST(weasel::IsKeyEventResultProcessed(eaten));
  BOOST_TEST(weasel::IsKeyEventResultEaten(eaten));

  weasel::UIStyle style;
  weasel::Status status;
  auto signature = RimeTrayIconSignature::From(style, status);
  BOOST_TEST(signature == RimeTrayIconSignature::From(style, status));

  weasel::Status composing_status = status;
  composing_status.composing = true;
  BOOST_TEST(signature == RimeTrayIconSignature::From(style, composing_status));

  weasel::Status ascii_status = status;
  ascii_status.ascii_mode = true;
  BOOST_TEST(signature != RimeTrayIconSignature::From(style, ascii_status));

  weasel::UIStyle icon_style = style;
  icon_style.current_zhung_icon = L"zh.ico";
  BOOST_TEST(signature != RimeTrayIconSignature::From(icon_style, status));

  RIME_STRUCT(RimeStatus, rime_status);
  rime_status.schema_id = "luna_pinyin";
  rime_status.schema_name = "Luna Pinyin";
  rime_status.is_ascii_mode = True;
  rime_status.is_composing = True;
  rime_status.is_disabled = False;
  rime_status.is_full_shape = True;

  auto status_snapshot = RimeUiStatusSnapshot::From(rime_status);
  BOOST_TEST(status_snapshot.has_status);
  BOOST_TEST(status_snapshot.schema_id == "luna_pinyin");
  BOOST_TEST(status_snapshot.status.schema_id == L"luna_pinyin");
  BOOST_TEST(status_snapshot.status.schema_name == L"Luna Pinyin");
  BOOST_TEST(status_snapshot.status.ascii_mode);
  BOOST_TEST(status_snapshot.status.composing);
  BOOST_TEST(!status_snapshot.status.disabled);
  BOOST_TEST(status_snapshot.status.full_shape);

  weasel::KeyEventTestCache key_cache;
  BOOST_TEST(!key_cache.Matches(false, 'A', 0x1e0001));
  key_cache.Store(false, 'A', 0x1e0001, FALSE);
  BOOST_TEST(key_cache.Matches(false, 'A', 0x1e0001));
  BOOST_TEST(!key_cache.Eaten());
  BOOST_TEST(!key_cache.Matches(true, 'A', 0x1e0001));
  BOOST_TEST(!key_cache.Matches(false, 'B', 0x1e0001));
  BOOST_TEST(!key_cache.Matches(false, 'A', 0x1f0001));
  key_cache.Store(true, 'A', 0x9e0001, TRUE);
  BOOST_TEST(key_cache.Matches(true, 'A', 0x9e0001));
  BOOST_TEST(key_cache.Eaten());
  key_cache.Clear();
  BOOST_TEST(!key_cache.Matches(true, 'A', 0x9e0001));

  weasel::Context current_context;
  weasel::Status current_status;
  BOOST_TEST(!RimeUiNeedsUpdate(current_context, current_status,
                                current_context, current_status));

  weasel::Context next_context = current_context;
  next_context.aux.str = L"tip";
  BOOST_TEST(RimeUiNeedsUpdate(current_context, current_status, next_context,
                               current_status));

  weasel::Status next_status = current_status;
  next_status.ascii_mode = true;
  BOOST_TEST(RimeUiNeedsUpdate(current_context, current_status,
                               current_context, next_status));

  BOOST_TEST(weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS, 0, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_SHUTDOWN_SERVER, 0, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_UPDATE_INPUT_POS, 0, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_TRAY_COMMAND, ID_WEASELTRAY_DEPLOY, 0}));
  BOOST_TEST(!weasel::ServerImpl::PipeMessageNeedsOuterApiLock(
      {WEASEL_IPC_TRAY_COMMAND, ID_WEASELTRAY_ENABLE_ASCII, 0}));

  return boost::report_errors();
}

bool launch_server() {
  int ret = (int)ShellExecute(NULL, L"open", L"TestWeaselIPC.exe", L"/start",
                              NULL, SW_NORMAL);
  if (ret <= 32) {
    std::cerr << "failed to launch server." << std::endl;
    return false;
  }
  return true;
}

bool read_buffer(LPWSTR buffer, UINT length, LPWSTR dest) {
  wbufferstream bs(buffer, length);
  bs.read(dest, WEASEL_IPC_BUFFER_LENGTH);
  return bs.good();
}

const char* wcstomb(const wchar_t* wcs) {
  const int buffer_len = 8192;
  static char buffer[buffer_len];
  WideCharToMultiByte(CP_OEMCP, NULL, wcs, -1, buffer, buffer_len, NULL, FALSE);
  return buffer;
}

int console_main() {
  weasel::Client client;
  if (!client.Connect()) {
    std::cerr << "failed to connect to server." << std::endl;
    return -2;
  }
  client.StartSession();
  if (!client.Echo()) {
    std::cerr << "failed to start session." << std::endl;
    return -3;
  }

  while (std::cin.good()) {
    int ch = std::cin.get();
    if (!std::cin.good())
      break;
    bool eaten = client.ProcessKeyEvent(weasel::KeyEvent(ch, 0));
    std::cout << "server replies: " << eaten << std::endl;
    if (eaten) {
      WCHAR response[WEASEL_IPC_BUFFER_LENGTH];
      bool ret = client.GetResponseData(
          std::bind<bool>(read_buffer, std::placeholders::_1,
                          std::placeholders::_2, std::ref(response)));
      std::cout << "get response data: " << ret << std::endl;
      std::cout << "buffer reads: " << std::endl
                << wcstomb(response) << std::endl;
    }
  }

  client.EndSession();

  return 0;
}

int client_main() {
  // launch_server();
  Sleep(1000);
  weasel::Client client;
  if (!client.Connect()) {
    std::cerr << "failed to connect to server." << std::endl;
    return -2;
  }
  client.StartSession();
  if (!client.Echo()) {
    std::cerr << "failed to login." << std::endl;
    return -3;
  }
  bool eaten = client.ProcessKeyEvent(weasel::KeyEvent(L'a', 0));
  std::cout << "server replies: " << eaten << std::endl;
  if (eaten) {
    WCHAR response[WEASEL_IPC_BUFFER_LENGTH];
    bool ret = client.GetResponseData(
        std::bind<bool>(read_buffer, std::placeholders::_1,
                        std::placeholders::_2, std::ref(response)));
    std::cout << "get response data: " << ret << std::endl;
    std::cout << "buffer reads: " << std::endl
              << wcstomb(response) << std::endl;
  }
  client.EndSession();

  system("pause");
  return 0;
}

class TestRequestHandler : public weasel::RequestHandler {
 public:
  TestRequestHandler() : m_counter(0) {
    std::cerr << "handler ctor." << std::endl;
  }
  virtual ~TestRequestHandler() {
    std::cerr << "handler dtor: " << m_counter << std::endl;
  }
  virtual UINT FindSession(UINT session_id) {
    std::cerr << "FindSession: " << session_id << std::endl;
    return (session_id <= m_counter ? session_id : 0);
  }
  virtual UINT AddSession(LPWSTR buffer) {
    std::cerr << "AddSession: " << m_counter + 1 << std::endl;
    return ++m_counter;
  }
  virtual UINT RemoveSession(UINT session_id) {
    std::cerr << "RemoveClient: " << session_id << std::endl;
    return 0;
  }
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               UINT session_id,
                               EatLine eat) {
    std::cerr << "ProcessKeyEvent: " << session_id
              << " keycode: " << keyEvent.keycode << " mask: " << keyEvent.mask
              << std::endl;
    eat(std::wstring(L"Greeting=Hello, 小狼毫.\n"));
    return TRUE;
  }

 private:
  unsigned int m_counter;
};

int server_main() {
  HRESULT hRes = _Module.Init(NULL, GetModuleHandle(NULL));
  ATLASSERT(SUCCEEDED(hRes));

  weasel::Server server;
  // weasel::UI ui;
  // const std::unique_ptr<weasel::RequestHandler> handler(new
  // RimeWithWeaselHandler(&ui));
  const std::unique_ptr<weasel::RequestHandler> handler(new TestRequestHandler);

  server.SetRequestHandler(handler.get());
  if (!server.Start())
    return -4;
  std::cerr << "server running." << std::endl;
  int ret = server.Run();
  std::cerr << "server quitting." << std::endl;
  return ret;
}
