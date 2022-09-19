#pragma once

#include <string>

#include <Windows.h>
#include <wininet.h>

class HttpClient
{
public:
	HttpClient(LPCTSTR lpszHost, bool https);
	bool HttpRequest(const std::string& path, const std::string& method, const std::string& headers, const std::string& body, std::string& response);

private:
	bool m_bHttps;
	HINTERNET m_hInternet;
	HINTERNET m_hConnect;
};
