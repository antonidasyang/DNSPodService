#include <fstream>
#include <iostream>

#include <windows.h>
#include <wininet.h>
#include <tchar.h>
#include <strsafe.h>

#include "json/json.h"
#include "resource.h"

#include <glog/logging.h>
#include <cpprest/http_client.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "jsoncpp.lib")
#pragma comment(lib, "glog.lib")

#define SVCNAME TEXT("Cloudflare Service")
#define HOST U("https://api.cloudflare.com/")
#define PATH_FMT "/client/v4/zones/%s/dns_records/%s"
#define CONTENT_FMT "{ \"name\": \"%s\", \"type\" : \"A\", \"content\" : \"%s\", \"ttl\" : 1 }"

namespace {
SERVICE_STATUS          gSvcStatus;
SERVICE_STATUS_HANDLE   gSvcStatusHandle;
HANDLE                  ghSvcStopEvent = NULL;
std::string             gPath;
}

VOID SvcInstall(void);
VOID SvcUninstall(void);
VOID WINAPI SvcCtrlHandler(DWORD);
VOID WINAPI SvcMain(DWORD, LPTSTR*);

VOID ReportSvcStatus(DWORD, DWORD, DWORD);
VOID SvcInit(DWORD, LPTSTR*);
VOID SvcReportEvent(LPTSTR);
bool UpdateDomain(void);
bool GetIP(std::wstring& ip);

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
    google::InitGoogleLogging("Cloudflare Service");

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
            if (UpdateDomain()) {
                LOG(INFO) << "UpdateDomain OK!";
                liDueTime.QuadPart = -600 * sec;
            }
            else {
                LOG(INFO) << "UpdateDomain Failed!";
                liDueTime.QuadPart = -60 * sec;
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
    LOG(INFO) << "Start UpdateDomain...";

    // get ip
    LOG(INFO) << "Getting ip...";
    std::wstring ip;
    if (!GetIP(ip)) {
        LOG(INFO) << "Get ip failed!";
        return false;
    }
    LOG(INFO) << "Get ip succeeded: " << ip.c_str();

    //LOG(INFO) << "Loading configuration file...";
    //Json::Value root;
    //std::ifstream ifs;
    //ifs.open(gPath + "\\config.json");
    //Json::CharReaderBuilder builder;
    //builder["collectComments"] = true;
    //JSONCPP_STRING errs;
    //if (!parseFromStream(builder, ifs, &root, &errs)) return false;

    //Json::Value dns_list = root["dns"];
    return false;

//    HINTERNET hInt = InternetOpen(NULL, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
//    if (hInt == NULL) return false;
//
//    HINTERNET hConnect = InternetConnect(hInt, HOST, INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
//    if (hConnect == NULL) {
//        InternetCloseHandle(hInt);
//        return false;
//    }
//
//    LPCSTR rgpszAcceptTypes[] = { "text/*", NULL };
//
//    for (Json::Value::ArrayIndex i = 0; i < dns_list.size(); ++i) {
//        std::string auth = dns_list[i]["auth"].asString();
//        std::string zone_id = dns_list[i]["zone_id"].asString();
//        std::string dns_id = dns_list[i]["dns_id"].asString();
//        std::string name = dns_list[i]["name"].asString();
//
//        web::http::client::http_client client(HOST);
//        web::http::uri_builder builder(U("/search"));
//        builder.append_query(U("q"), U("cpprestsdk github"));
//        web::http::http_headers headers;
//        headers.add(U("Authorization"), U("Bearer "));
//        return client.request(methods::GET, builder.to_string());
////        HttpClient hc(HOST, true);
//        char szPath[MAX_PATH];
//        sprintf_s(szPath, PATH_FMT, zone_id.c_str(), dns_id.c_str());
//
//        std::string headers = "Authorization: Bearer " + auth + "\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nAccept: */*\r\n";
//        char szContent[1000];
//        memset(szContent, 0, 1000);
//        sprintf_s(szContent, CONTENT_FMT, name.c_str(), ip.c_str());
//        std::string response;
//        LOG(WARNING) << "HEADERS: " << headers;
//        LOG(WARNING) << "CONTENT: " << szContent;
//        if (hc.HttpRequest(szPath, "PUT", headers, szContent, response)) {
//            Json::Value result;
//            std::istringstream iss(response);
//            Json::CharReaderBuilder resultBuilder;
//            resultBuilder["collectComments"] = true;
//            if (parseFromStream(resultBuilder, iss, &result, &errs)) {
//                if (result["success"].asBool()) {
//                    LOG(INFO) << "Update " << ip << " to dns " << name << " succeeded!";
//                    continue;
//                }
//            }
//
//            LOG(ERROR) << "Update " << ip << " to dns " << name << " failed!";
//            LOG(ERROR) << "Resposne: " << response;
//        }
//    }
//
//    return true;
}

bool GetIP(std::wstring& ip) {
    LOG(INFO) << "1111111111";
    Sleep(100);
    web::http::client::http_client client(U("http://www.net.cn/"));
    LOG(INFO) << "222222222";
    Sleep(100);
    web::http::uri_builder builder(U("/static/customercare/yourip.asp"));
    LOG(INFO) << "3333333333";
    Sleep(100);
    web::http::http_response response = client.request(web::http::methods::GET, builder.to_string()).get();
    LOG(INFO) << "4444444444";
    Sleep(100);
    //response.body().read_to_end(fileStream->streambuf());
    std::wstring body = response.extract_string().get();
    LOG(INFO) << "5555555555";
    Sleep(100);

    size_t p1 = body.find(L"<h2>");
    if (p1 == std::string::npos) return false;
    p1 += 4;

    size_t p2 = body.find(L"</h2>");
    if (p2 == std::string::npos) return false;

    ip = body.substr(p1, p2 - p1);
    return true;
}
