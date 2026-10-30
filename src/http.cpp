#include "http.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace ab {
namespace {

std::string win_error(const char *what) {
    DWORD code = GetLastError();
    char *msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPSTR)&msg, 0, nullptr);
    std::string s = what;
    s += ": ";
    if (msg) {
        s += msg;
        LocalFree(msg);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
            s.pop_back();
    } else {
        s += "error " + std::to_string(code);
    }
    return s;
}

std::wstring to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

}  // namespace

std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool http_request(const std::string &url, const std::string &method,
                  const std::string &body, const std::string &content_type,
                  HttpResponse &resp) {
    resp = HttpResponse();
    std::wstring wurl = to_wide(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        resp.error = win_error("WinHttpCrackUrl");
        return false;
    }

    HINTERNET session = WinHttpOpen(L"argonmine-gpu/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        resp.error = win_error("WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) {
        resp.error = win_error("WinHttpConnect");
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    std::wstring wmethod = to_wide(method);
    HINTERNET request = WinHttpOpenRequest(connect, wmethod.c_str(), path, nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        resp.error = win_error("WinHttpOpenRequest");
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring headers;
    if (!content_type.empty())
        headers = L"Content-Type: " + to_wide(content_type) +
                  L"\r\nAccept: application/json\r\n";
    else
        headers = L"Accept: application/json\r\n";

    BOOL ok;
    if (!body.empty()) {
        ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L,
                                (LPVOID)body.data(), (DWORD)body.size(),
                                (DWORD)body.size(), 0);
    } else {
        ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }
    if (!ok) {
        resp.error = win_error("WinHttpSendRequest");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        resp.error = win_error("WinHttpReceiveResponse");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0, slen = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    resp.status = (int)status;

    char buf[8192];
    DWORD got = 0;
    while (WinHttpReadData(request, buf, sizeof(buf), &got) && got > 0) {
        resp.body.append(buf, got);
        if (resp.body.size() > (size_t)4 << 20) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

}  // namespace ab
