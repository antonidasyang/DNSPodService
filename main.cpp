#include <fstream>
#include <iostream>

#include <windows.h>
#include <wininet.h>
#include <tchar.h>
#include <strsafe.h>

#include "json/json.h"
#include "resource.h"

#include <glog/logging.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "jsoncpp.lib")
#pragma comment(lib, "glog.lib")

#define SVCNAME TEXT("DNSPod Service")

SERVICE_STATUS          gSvcStatus;
SERVICE_STATUS_HANDLE   gSvcStatusHandle;
HANDLE                  ghSvcStopEvent = NULL;
std::string             gPath;
std::string             gLogin;
std::string             gName;
std::string             gDomain;


VOID SvcInstall(void);
VOID SvcUninstall(void);
VOID WINAPI SvcCtrlHandler(DWORD);
VOID WINAPI SvcMain(DWORD, LPTSTR*);

VOID ReportSvcStatus(DWORD, DWORD, DWORD);
VOID SvcInit(DWORD, LPTSTR*);
VOID SvcReportEvent(LPTSTR);
bool UpdateDomain(void);
bool HttpRequest(const std::string& url, const std::string& method, const std::string& post_data, std::string& content);
void WriteFile(const std::string& filename, const std::string& content);
bool LoadConfig();


int __cdecl _tmain(int argc, TCHAR* argv[]) {
    // If command-line parameter is "install", install the service. 
    // Otherwise, the service is probably being started by the SCM.
    if (lstrcmpi(argv[1], TEXT("install")) == 0) {
        SvcInstall();

        return 0;
    }

    // If command-line parameter is "uninstall", uninstall the service. 
    // Otherwise, the service is probably being started by the SCM.
    if (lstrcmpi(argv[1], TEXT("uninstall")) == 0) {
        SvcUninstall();

        return 0;
    }

    char buffer[1001];
    int len = WideCharToMultiByte(CP_ACP, 0, argv[0], (int) wcslen(argv[0]), buffer, 1000, NULL, NULL);
    buffer[len] = '\0';
    gPath = buffer;
    size_t p = gPath.find_last_of('\\');
    if (p != std::string::npos) gPath = gPath.substr(0, p);
    std::string log_path = gPath + "\\logs";
    CreateDirectoryA(log_path.c_str(), NULL);
    FLAGS_log_dir = log_path;
    google::InitGoogleLogging("DNSPod Service");

    // TO_DO: Add any additional services for the process to this table.
    SERVICE_TABLE_ENTRY DispatchTable[] =
    {
        { const_cast<LPTSTR>(SVCNAME), (LPSERVICE_MAIN_FUNCTION) SvcMain },
        { NULL, NULL }
    };

    // This call returns when the service has stopped. 
    // The process should simply terminate when the call returns.
    if (!StartServiceCtrlDispatcher(DispatchTable)) {
        SvcReportEvent(const_cast<LPTSTR>(TEXT("StartServiceCtrlDispatcher")));
    }

    google::ShutdownGoogleLogging();
    return 0;
}

VOID SvcInstall() {
    TCHAR szPath[MAX_PATH];
    if (!GetModuleFileName(NULL, szPath, MAX_PATH)) {
        std::cout << "Cannot install service (" << GetLastError() << ")" << std::endl;
        return;
    }

    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                    // local computer
        NULL,                    // ServicesActive database 
        SC_MANAGER_ALL_ACCESS);  // full access rights 

    if (schSCManager == NULL) {
        std::cout << "Open SCManager failed (" << GetLastError() << ")" << std::endl;
        return;
    }

    // Create the service
    SC_HANDLE schService = CreateService(
        schSCManager,              // SCM database 
        SVCNAME,                   // name of service 
        SVCNAME,                   // service name to display 
        SERVICE_ALL_ACCESS,        // desired access 
        SERVICE_WIN32_OWN_PROCESS, // service type 
        SERVICE_DEMAND_START,      // start type 
        SERVICE_ERROR_NORMAL,      // error control type 
        szPath,                    // path to service's binary 
        NULL,                      // no load ordering group 
        NULL,                      // no tag identifier 
        NULL,                      // no dependencies 
        NULL,                      // LocalSystem account 
        NULL);                     // no password 

    if (schService == NULL) {
        std::cout << "Create Service failed (" << GetLastError() << ")" << std::endl;
        CloseServiceHandle(schSCManager);
        return;
    }
    else std::cout << "Service installed successfully" << std::endl;

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}

VOID SvcUninstall() {
    // Get a handle to the SCM database. 
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                    // local computer
        NULL,                    // ServicesActive database 
        SC_MANAGER_ALL_ACCESS);  // full access rights 

    if (schSCManager == NULL) {
        std::cout << "Open SCManager failed (" << GetLastError() << ")" << std::endl;
        return;
    }

    // Get a handle to the service.
    SC_HANDLE schService = OpenService(
        schSCManager,       // SCM database 
        SVCNAME,            // name of service 
        DELETE);            // need delete access 

    if (schService == NULL) {
        std::cout << "Open Service failed (" << GetLastError() << ")" << std::endl;
        CloseServiceHandle(schSCManager);
        return;
    }

    // Delete the service.
    if (!DeleteService(schService)) {
        std::cout << "Delete Service failed (" << GetLastError() << ")" << std::endl;
    }
    else std::cout << "Service deleted successfully" << std::endl;

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}

VOID WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv) {
    // Register the handler function for the service
    gSvcStatusHandle = RegisterServiceCtrlHandler(
        SVCNAME,
        SvcCtrlHandler);

    if (!gSvcStatusHandle) {
        SvcReportEvent(const_cast<LPTSTR>(TEXT("RegisterServiceCtrlHandler")));
        return;
    }

    // These SERVICE_STATUS members remain as set here
    gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gSvcStatus.dwServiceSpecificExitCode = 0;

    // Report initial status to the SCM
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // Perform service-specific initialization and work.
    SvcInit(dwArgc, lpszArgv);
}

VOID SvcInit(DWORD dwArgc, LPTSTR* lpszArgv) {
    // TO_DO: Declare and set any required variables.
    //   Be sure to periodically call ReportSvcStatus() with 
    //   SERVICE_START_PENDING. If initialization fails, call
    //   ReportSvcStatus with SERVICE_STOPPED.

    // Create an event. The control handler function, SvcCtrlHandler,
    // signals this event when it receives the stop control code.
    ghSvcStopEvent = CreateEvent(
        NULL,    // default security attributes
        TRUE,    // manual reset event
        FALSE,   // not signaled
        NULL);   // no name

    if (ghSvcStopEvent == NULL) {
        ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return;
    }

    // Report running status when initialization is complete.
    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

    // TO_DO: Perform work until service stops.
    HANDLE hTimer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (hTimer == NULL) {
        return;
    }

    LONGLONG sec = 10000000LL;
    LARGE_INTEGER liDueTime;
    liDueTime.QuadPart = -3 * sec;

    HANDLE handles[2];
    handles[0] = ghSvcStopEvent;
    handles[1] = hTimer;

    if (!SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, 0)) {
        return;
    }

    while (true) {
        DWORD ret = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (ret == WAIT_OBJECT_0) {
            CancelWaitableTimer(hTimer);
            break;
        }
        else if (ret == (WAIT_OBJECT_0 + 1)) {
            if (LoadConfig()) {
                if (UpdateDomain()) {
                    LOG(INFO) << "UpdateDomain OK!";
                    liDueTime.QuadPart = -600 * sec;
                }
                else {
                    LOG(INFO) << "UpdateDomain Failed!";
                    liDueTime.QuadPart = -60 * sec;
                }
            }

            SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, 0);
        }
    }

    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
}

VOID ReportSvcStatus(DWORD dwCurrentState,
    DWORD dwWin32ExitCode,
    DWORD dwWaitHint) {
    static DWORD dwCheckPoint = 1;

    // Fill in the SERVICE_STATUS structure.
    gSvcStatus.dwCurrentState = dwCurrentState;
    gSvcStatus.dwWin32ExitCode = dwWin32ExitCode;
    gSvcStatus.dwWaitHint = dwWaitHint;

    if (dwCurrentState == SERVICE_START_PENDING)
        gSvcStatus.dwControlsAccepted = 0;
    else gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    if ((dwCurrentState == SERVICE_RUNNING) ||
        (dwCurrentState == SERVICE_STOPPED))
        gSvcStatus.dwCheckPoint = 0;
    else gSvcStatus.dwCheckPoint = dwCheckPoint++;

    // Report the status of the service to the SCM.
    SetServiceStatus(gSvcStatusHandle, &gSvcStatus);
}

VOID WINAPI SvcCtrlHandler(DWORD dwCtrl) {
    // Handle the requested control code. 
    switch (dwCtrl) {
    case SERVICE_CONTROL_STOP:
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

        // Signal the service to stop.
        SetEvent(ghSvcStopEvent);
        ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);

        return;

    case SERVICE_CONTROL_INTERROGATE:
        break;

    default:
        break;
    }

}

VOID SvcReportEvent(LPTSTR szFunction) {
    HANDLE hEventSource;
    LPCTSTR lpszStrings[2];
    TCHAR Buffer[80];

    hEventSource = RegisterEventSource(NULL, SVCNAME);

    if (hEventSource != NULL) {
        StringCchPrintf(Buffer, 80, TEXT("%s failed with %d"), szFunction, GetLastError());

        lpszStrings[0] = SVCNAME;
        lpszStrings[1] = Buffer;

        ReportEvent(
            hEventSource,        // event log handle
            EVENTLOG_ERROR_TYPE, // event type
            0,                   // event category
            SVC_ERROR,           // event identifier
            NULL,                // no security identifier
            2,                   // size of lpszStrings array
            0,                   // no binary data
            lpszStrings,         // array of strings
            NULL);               // no binary data

        DeregisterEventSource(hEventSource);
    }
}

bool UpdateDomain() {
    std::string content;
    std::string post_data;

    // get record id
    post_data = "login_token=" + gLogin + "&format=json&domain=" + gDomain;
    if (!HttpRequest("https://dnsapi.cn/Record.List", "POST", post_data, content)) {
        LOG(ERROR) << "POST FAILED: https://dnsapi.cn/Record.List - " << post_data;
        LOG(ERROR) << content;
        return false;
    }

    JSONCPP_STRING err;
    Json::Value root;
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(content.c_str(), content.c_str() + content.size(), &root, &err)) {
        LOG(ERROR) << "PARSE FAILED:" << content;
        LOG(ERROR) << "https://dnsapi.cn/Record.List - " << post_data;
        return false;
    }

    auto status = root["status"];
    if (status["code"].asString().compare("1") != 0) return false;

    auto records = root["records"];
    std::string id;
    std::string type;
    std::string line_id;
    for (auto record = records.begin(); record != records.end(); ++record) {
        if ((*record)["name"].asString().compare("bj") == 0) {
            id = (*record)["id"].asString();
            type = (*record)["type"].asString();
            line_id = (*record)["line_id"].asString();
            break;
        }
    }
    if (id.empty()) return false;

    // get ip
    if (!HttpRequest("http://www.net.cn/static/customercare/yourip.asp", "GET", "", content)) {
        LOG(ERROR) << "GET FAILED: http://www.net.cn/static/customercare/yourip.asp";
        return false;
    }

    size_t p1 = content.find("<h2>");
    if (p1 == std::string::npos) return false;
    p1 += 4;

    size_t p2 = content.find("</h2>");
    if (p2 == std::string::npos) return false;

    std::string ip = content.substr(p1, p2 - p1);

    // modify
    post_data = "login_token=" + gLogin + "&format=json&domain=d2ssoft.com&record_id=" + id + "&sub_domain=" + gName + "&value=" + ip + "&record_type=" + type + "&record_line_id=" + line_id;
    if (!HttpRequest("https://dnsapi.cn/Record.Modify", "POST", post_data, content)) {
        LOG(ERROR) << "POST FAILED: https://dnsapi.cn/Record.Modify - " << post_data;
        return false;
    }

    if (!reader->parse(content.c_str(), content.c_str() + content.size(), &root, &err)) {
        LOG(ERROR) << "PARSE FAILED:" << content;
        LOG(ERROR) << "https://dnsapi.cn/Record.Modify - " << post_data;
        return false;
    }
    LOG(INFO) << "MODIFY RESULT: " << content;

    status = root["status"];
    return status["code"].asString().compare("1") == 0;
}

bool HttpRequest(const std::string& url, const std::string& method, const std::string& post_data, std::string& content) {
    if (url.compare(0, 4, "http") != 0) return false;

    size_t p1 = url.find("://", 0);
    if (p1 == std::string::npos) return false;

    p1 += 3;
    size_t p2 = url.find("/", p1);
    std::string host;
    std::string route = "/";
    if (p2 == std::string::npos) {
        host = url.substr(p1);
    }
    else {
        host = url.substr(p1, p2 - p1);
        route = url.substr(p2);
    }

    HINTERNET hInt = InternetOpenA("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/76.0.3785.3 Safari/537.36", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (hInt == NULL) return false;

    bool https = (url.compare(0, 5, "https") == 0);
    HINTERNET hConnect = InternetConnectA(hInt, host.c_str(), https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (hConnect == NULL) {
        InternetCloseHandle(hInt);
        return false;
    }

    LPCSTR rgpszAcceptTypes[] = { "text/*", NULL };
    DWORD dwFlags = INTERNET_FLAG_RELOAD;
    if (https) dwFlags |= INTERNET_FLAG_SECURE;
    HINTERNET hRequest = HttpOpenRequestA(hConnect, method.c_str(), route.c_str(), "HTTP/1.1", NULL, rgpszAcceptTypes, dwFlags, 0);
    if (hRequest == NULL) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInt);
        return false;
    }

    std::string headers = "Host: " + host + "\r\nConnection: keep-alive\r\n";
    BOOL ret = FALSE;
    if (method.compare("POST") == 0) {
        headers += "Content-Type: application/x-www-form-urlencoded\r\n";
        ret = HttpSendRequestA(hRequest, headers.c_str(), -1L, (LPVOID)post_data.c_str(), (DWORD)post_data.size());
    }
    else {
        ret = HttpSendRequestA(hRequest, headers.c_str(), -1L, NULL, 0);
    }

    if (ret == TRUE) {
        char szData[1001];
        DWORD dwSize = 0;
        DWORD dwTotal = 0;
        content = "";
        while (InternetReadFile(hRequest, (LPVOID)szData, 1000, &dwSize) == TRUE) {
            if (dwSize == 0) break;

            szData[dwSize] = '\0';
            content += szData;
        }

    }

    HttpEndRequest(hRequest, NULL, 0, 0);
    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInt);

    return ret == TRUE;
}

void WriteFile(const std::string& filename, const std::string& content) {
    std::ofstream ofs(filename, std::ofstream::out);
    ofs.write(content.c_str(), content.size());
    ofs.close();
}

bool LoadConfig() {
    Json::Value root;
    std::ifstream ifs;
    ifs.open(gPath + "\\config.json");
    Json::CharReaderBuilder builder;
    builder["collectComments"] = true;
    JSONCPP_STRING errs;
    if (!parseFromStream(builder, ifs, &root, &errs)) return false;

    gLogin = root["login"].asString();
    gName = root["name"].asString();
    gDomain = root["domain"].asString();

    return true;
}