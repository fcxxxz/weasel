// TestResponseParser.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <boost/archive/text_woarchive.hpp>
#include <boost/detail/lightweight_test.hpp>
#include <ResponseParser.h>
#include <sstream>
#include <string>

void test_1() {
  WCHAR resp[] = L"action=noop\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(commit.empty());
  BOOST_TEST(ctx.empty());
}

void test_2() {
  WCHAR resp[] =
      L"action=commit\n"
      L"commit=教這句話上屏=3.14\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  ctx.aux.str = L"從前的值";
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(commit == L"教這句話上屏=3.14");
  BOOST_TEST(ctx.preedit.empty());
  BOOST_TEST(ctx.aux.str == L"從前的值");
  BOOST_TEST(ctx.cinfo.candies.empty());
}

void test_3() {
  WCHAR resp[] =
      L"action=ctx\n"
      L"ctx.preedit=寫作串=3.14\n"
      L"ctx.aux=sie'zuoh'chuan=3.14\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(commit.empty());
  BOOST_TEST(ctx.preedit.str == L"寫作串=3.14");
  BOOST_TEST(ctx.preedit.attributes.empty());
  BOOST_TEST(ctx.aux.str == L"sie'zuoh'chuan=3.14");
}

void test_4() {
  // Candidates travel as one boost text-archive payload on ctx.cand=, the
  // way RimeWithWeasel::_Respond actually serializes CandidateInfo.
  weasel::CandidateInfo sent;
  sent.currentPage = 0;
  sent.totalPages = 1;
  sent.highlighted = 1;
  sent.is_last_page = true;
  sent.candies.push_back(weasel::Text(L"候選甲"));
  sent.candies.push_back(weasel::Text(L"候選乙"));
  std::wostringstream oss;
  {
    boost::archive::text_woarchive oa(oss);
    oa << sent;
  }
  const std::wstring resp = std::wstring(
                                L"action=commit,ctx\n"
                                L"ctx.preedit=候選乙=3.14\n"
                                L"ctx.preedit.cursor=0,3\n"
                                L"ctx.cand=") +
                            oss.str() + L"\n";
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  weasel::ResponseParser parser(&commit, &ctx, &status);
  std::vector<WCHAR> buf(resp.begin(), resp.end());
  parser(buf.data(), (UINT)buf.size());
  weasel::CandidateInfo& c = ctx.cinfo;
  BOOST_TEST(commit.empty());
  BOOST_TEST(ctx.preedit.str == L"候選乙=3.14");
  BOOST_ASSERT(1 == ctx.preedit.attributes.size());
  weasel::TextAttribute attr0 = ctx.preedit.attributes[0];
  BOOST_TEST_EQ(weasel::HIGHLIGHTED, attr0.type);
  BOOST_TEST_EQ(0, attr0.range.start);
  BOOST_TEST_EQ(3, attr0.range.end);
  BOOST_TEST(ctx.aux.empty());
  BOOST_ASSERT(2 == c.candies.size());
  BOOST_TEST(c.candies[0].str == L"候選甲");
  BOOST_TEST(c.candies[1].str == L"候選乙");
  BOOST_TEST_EQ(1, c.highlighted);
  BOOST_TEST_EQ(0, c.currentPage);
  BOOST_TEST_EQ(1, c.totalPages);
}

void test_5() {
  // A truncated/garbage ctx.cand payload must reset the candidate state
  // instead of crashing the process (regression guard for the old
  // MessageBoxA / escaped-exception behavior).
  WCHAR resp[] =
      L"action=ctx\n"
      L"ctx.cand=not-an-archive\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  ctx.cinfo.candies.push_back(weasel::Text(L"舊候選"));
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(ctx.cinfo.candies.empty());
}

void test_6() {
  // A config-only response must update the config even when the caller
  // passes no Context (e.g. OnSetThreadFocus). Regression guard: the
  // Configurator used to bail out on !p_context and drop every field.
  WCHAR resp[] =
      L"action=config\n"
      L"config.inline_preedit=1\n"
      L"config.hide_ime_mode_icon=1\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Status status;
  weasel::Config config;
  weasel::ResponseParser parser(&commit, NULL, &status, &config);
  parser(resp, len);
  BOOST_TEST(config.inline_preedit == true);
  BOOST_TEST(config.hide_ime_mode_icon == true);
}

void test_7() {
  // The server must send the candidate archive on every composing response:
  // the TSF client decodes each response into a fresh Context, so an omitted
  // ctx.cand line means "empty candidates" on the client. Both consecutive
  // responses here carry the same unchanged archive; decoding each into its
  // own fresh Context must retain the candidates both times.
  weasel::CandidateInfo sent;
  sent.currentPage = 0;
  sent.totalPages = 1;
  sent.highlighted = 0;
  sent.is_last_page = true;
  sent.candies.push_back(weasel::Text(L"同一頁"));
  std::wostringstream oss;
  {
    boost::archive::text_woarchive oa(oss);
    oa << sent;
  }
  const std::wstring one =
      std::wstring(L"action=ctx\nctx.preedit=tong\nctx.cand=") + oss.str() +
      L"\n";

  for (int round = 0; round < 2; ++round) {
    std::wstring commit;
    weasel::Context ctx;  // fresh Context per response, like DoEditSession
    weasel::Status status;
    weasel::ResponseParser parser(&commit, &ctx, &status);
    std::vector<WCHAR> buf(one.begin(), one.end());
    parser(buf.data(), (UINT)buf.size());
    BOOST_ASSERT(1 == ctx.cinfo.candies.size());
    BOOST_TEST(ctx.cinfo.candies[0].str == L"同一頁");
    BOOST_TEST(ctx.preedit.str == L"tong");
  }
}

int _tmain(int argc, _TCHAR* argv[]) {
  test_1();
  test_2();
  test_3();
  test_4();
  test_5();
  test_6();
  test_7();

  return boost::report_errors();
}
