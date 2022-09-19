#include "HttpClient.h"

#include "glog/logging.h"

HttpClient::HttpClient(LPCTSTR lpszHost, bool https) :
    m_bHttps(false),
    m_hInternet(NULL),
    m_hConnect(NULL) {

    m_bHttps = https;
    m_hInternet = InternetOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/76.0.3785.3 Safari/537.36", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    m_hConnect = InternetConnect(m_hInternet, lpszHost, m_bHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (m_hConnect == NULL) m_hInternet = NULL;
}

bool HttpClient::HttpRequest(const std::string& path, const std::string& method, const std::string& headers, const std::string& body, std::string& response) {
    if (m_hConnect == NULL) return false;

    LPCSTR rgpszAcceptTypes[] = { "text/*", NULL };
    DWORD dwFlags = INTERNET_FLAG_RELOAD;
    LOG(INFO) << "method: " << method;
    LOG(INFO) << "path: " << path;
    HINTERNET hRequest = HttpOpenRequestA(m_hConnect, method.c_str(), path.c_str(), "HTTP/1.1", NULL, rgpszAcceptTypes, dwFlags, 0);

    BOOL ret = HttpSendRequestA(hRequest, headers.c_str(), -1L, (LPVOID)body.c_str(), (DWORD)body.size());
    response = "";
    if (ret == TRUE) {
        char szData[1001];
        DWORD dwSize = 0;
        DWORD dwTotal = 0;
        while (InternetReadFile(hRequest, (LPVOID)szData, 1000, &dwSize) == TRUE) {
            if (dwSize == 0) break;

            szData[dwSize] = '\0';
            response += szData;
        }
    }

    HttpEndRequest(hRequest, NULL, 0, 0);
    InternetCloseHandle(hRequest);

    return ret == TRUE;
}
