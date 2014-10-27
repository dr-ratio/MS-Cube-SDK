#include "stdafx.h"
#include "resource.h"

#include "Kinect.h"
#include <winsock.h>
#include <string>
#include <process.h>
#include <ctime>

#define TRAYICONID	1//				ID number for the Notify Icon
#define SWM_TRAYMSG	WM_APP//		the message ID sent to our window

#define SWM_SHOW	WM_APP + 1//	show the window
#define SWM_EXIT	WM_APP + 2//	close the window

// Global Variables:
HINSTANCE		hInst;	// current instance
NOTIFYICONDATA	niData;	// notify icon data

// Global Kinect Variables and functions
IKinectSensor*				kinectSensor;
bool						fKinectConnected;
ICoordinateMapper*			coordinateMapper;
IBodyFrameReader*			bodyFrameReader;
IDepthFrameReader*			depthFrameReader;
WAITABLE_HANDLE				bodyFrameEvent;
WAITABLE_HANDLE				depthFrameEvent;
const int					cDepthWidth  = 512;
const int					cDepthHeight = 424;
RGBQUAD*					pDepthRGBX;
char*						pDepthFrame;
std::string					strDestinationHost;
std::string					strConnectedHost;
bool						fSendSkeletonData;
bool						fSendDepthData;

bool UpdateKinect();
bool UpdateKinectSkeleton();
bool UpdateKinectDepth();
unsigned int __stdcall KinectThread(void* data);

// Global Socket Variables and functions
SOCKET hSocket;
bool ConnectToHost(int PortNo, const char* IPAddress);
void CloseConnection();
bool SendSkeletonUpdate(IBody** ppBodies);
bool SendDepthUpdate(int nWidth, int nHeight, UINT16 *pBuffer, USHORT nMinDepth, USHORT nMaxDepth);

// Forward declarations of functions included in this code module:
BOOL				InitInstance(HINSTANCE, int);
BOOL				OnInitDialog(HWND hWnd);
void				ShowContextMenu(HWND hWnd);
ULONGLONG			GetDllVersion(LPCTSTR lpszDllName);

INT_PTR CALLBACK	DlgProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK	About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR    lpCmdLine,
                     int       nCmdShow)
{
	MSG msg;
	HACCEL hAccelTable;

	// Perform application initialization:
	if (!InitInstance (hInstance, nCmdShow)) return FALSE;
	hAccelTable = LoadAccelerators(hInstance, (LPCTSTR)IDC_KINECTTRANSPORT);

	// setup Kinect
	HRESULT hr = GetDefaultKinectSensor(&kinectSensor);
    if (SUCCEEDED(hr) && kinectSensor)
    {
        hr = kinectSensor->Open();
        if (SUCCEEDED(hr))
            hr = kinectSensor->get_CoordinateMapper(&coordinateMapper);

		// setup body frame
		IBodyFrameSource* pBodyFrameSource = NULL;
        if (SUCCEEDED(hr))
            hr = kinectSensor->get_BodyFrameSource(&pBodyFrameSource);
        if (SUCCEEDED(hr))
            hr = pBodyFrameSource->OpenReader(&bodyFrameReader);
		if (SUCCEEDED(hr))
			hr = bodyFrameReader->SubscribeFrameArrived(&bodyFrameEvent);
		if (pBodyFrameSource != NULL)
			pBodyFrameSource->Release();

		// setup depth frame
		IDepthFrameSource* pDepthFrameSource = NULL;
		if (SUCCEEDED(hr))
            hr = kinectSensor->get_DepthFrameSource(&pDepthFrameSource);
        if (SUCCEEDED(hr))
            hr = pDepthFrameSource->OpenReader(&depthFrameReader);
		if (SUCCEEDED(hr))
			hr = depthFrameReader->SubscribeFrameArrived(&depthFrameEvent);
		if (pDepthFrameSource != NULL)
			pDepthFrameSource->Release();
    }

	// create heap storage for depth pixel data in RGBX format
    pDepthRGBX = new RGBQUAD[cDepthWidth * cDepthHeight];

	// maximum size of frame is 247815 bytes (512x424 + 7)
	pDepthFrame = new char[247815];
	fKinectConnected = false;
	HANDLE kinectThreadHandle = (HANDLE)_beginthreadex(0, 0, &KinectThread, 0, 0, 0);
	SetThreadPriority(kinectThreadHandle, THREAD_PRIORITY_TIME_CRITICAL);

	// Main message loop:
	while (true)
	{
		MSG msg;
		while( ::PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) {
			::TranslateMessage( &msg );
			::DispatchMessage( &msg );
		}
	}

	return (int) msg.wParam;
}

unsigned int __stdcall KinectThread(void* data)
{
	while (true)
	{
		// try to connect if we aren't connected
		if (!fKinectConnected || strDestinationHost != strConnectedHost)
		{
			fKinectConnected = ConnectToHost(3000, strDestinationHost.c_str());
			if (fKinectConnected)
				strConnectedHost = strDestinationHost;
		}
		
		if (fKinectConnected)
		{
			fKinectConnected = UpdateKinect();
		}
	}

	if (fKinectConnected)
		CloseConnection();

	return 0;
}


bool GetBoolRegValue(HKEY hKey, const std::string &strValueName, bool &bValue, bool bDefaultValue)
{
    DWORD nDefValue((bDefaultValue) ? 1 : 0);
    DWORD nResult(nDefValue);
	DWORD dwBufferSize(sizeof(DWORD));
    LONG nError = ::RegQueryValueEx(hKey,
        strValueName.c_str(),
        0,
        NULL,
        reinterpret_cast<LPBYTE>(&nResult),
        &dwBufferSize);
    if (ERROR_SUCCESS == nError)
    {
        return (nResult != 0) ? true : false;
    }
    return (ERROR_SUCCESS == nError);
}


LONG GetStringRegValue(HKEY hKey, const std::string &strValueName, std::string &strValue, const std::string &strDefaultValue)
{
    strValue = strDefaultValue;
    CHAR szBuffer[512];
    DWORD dwBufferSize = sizeof(szBuffer);
    ULONG nError;
    nError = RegQueryValueEx(hKey, strValueName.c_str(), 0, NULL, (LPBYTE)szBuffer, &dwBufferSize);
    if (ERROR_SUCCESS == nError)
    {
        strValue = szBuffer;
    }
    return (ERROR_SUCCESS == nError);
}

bool CreateRegistryKey(HKEY hKeyRoot, LPCTSTR pszSubKey, HKEY &hNewKey)
{
    DWORD dwFunc;
    LONG  lRet;
    SECURITY_DESCRIPTOR SD;
    SECURITY_ATTRIBUTES SA;

    if(!InitializeSecurityDescriptor(&SD, SECURITY_DESCRIPTOR_REVISION))
        return false;
    if(!SetSecurityDescriptorDacl(&SD, true, 0, false))
        return false;

    SA.nLength             = sizeof(SA);
    SA.lpSecurityDescriptor = &SD;
    SA.bInheritHandle      = false;
    lRet = RegCreateKeyEx(
        hKeyRoot,
        pszSubKey,
        0,
        (LPTSTR)NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        &SA,
        &hNewKey,
        &dwFunc
    );

    if(lRet == ERROR_SUCCESS)
        return true;

    SetLastError((DWORD)lRet);
    return false;
}

bool LoadFromRegistry()
{
	bool bValue;
	std::string strValue;

	HKEY hKey;
	LONG lRes = RegOpenKeyEx(HKEY_CURRENT_USER, "SOFTWARE\\KinectTransport", 0, KEY_READ, &hKey);
	if (lRes != ERROR_SUCCESS)
		return false;

	if (GetStringRegValue(hKey, "DestinationHost", strValue, "127.0.0.1"))
		strDestinationHost = strValue;
	else
		return false;
	if (GetBoolRegValue(hKey, "SendSkeletonData", bValue, true))
		fSendSkeletonData = bValue;
	else
		return false;
	if (GetBoolRegValue(hKey, "SendDepthData", bValue, true))
		fSendDepthData = bValue;
	else
		return false;

	RegCloseKey(hKey);
	return true;
}

bool SaveToRegistry()
{
	HKEY hKey;
	LONG lRes = RegOpenKeyEx(HKEY_CURRENT_USER, "SOFTWARE\\KinectTransport", 0, KEY_READ | KEY_SET_VALUE, &hKey);
	if (lRes != ERROR_SUCCESS)
	{
		// no key, lets create one
		HKEY hSoftwareKey;
		lRes = RegOpenKeyEx(HKEY_CURRENT_USER, "SOFTWARE", 0, KEY_READ | KEY_SET_VALUE, &hSoftwareKey);
		if (lRes != ERROR_SUCCESS || !CreateRegistryKey(hSoftwareKey, "KinectTransport", hKey))
			return false;
	}

	// now save our values
	lRes = RegSetValueEx(hKey, "DestinationHost", 0, REG_SZ, (unsigned char*)strDestinationHost.c_str(), strDestinationHost.length() * sizeof(TCHAR));
	DWORD dValue = fSendSkeletonData ? 1 : 0;
	lRes = RegSetValueEx(hKey, "SendSkeletonData", 0, REG_DWORD, (unsigned char*)&dValue, sizeof(DWORD));
	dValue = fSendDepthData ? 1 : 0;
	lRes = RegSetValueEx(hKey, "SendDepthData", 0, REG_DWORD, (unsigned char*)&dValue, sizeof(DWORD));
	RegCloseKey(hKey);

	return true;
}

//	Initialize the window and tray icon
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	// prepare for XP style controls
	InitCommonControls();

	 // store instance handle and create dialog
	hInst = hInstance;
	HWND hWnd = CreateDialog( hInstance, MAKEINTRESOURCE(IDD_DLG_DIALOG),
		NULL, (DLGPROC)DlgProc );
	if (!hWnd) return FALSE;

	// Fill the NOTIFYICONDATA structure and call Shell_NotifyIcon

	// zero the structure - note:	Some Windows funtions require this but
	//								I can't be bothered which ones do and
	//								which ones don't.
	ZeroMemory(&niData,sizeof(NOTIFYICONDATA));

	// get Shell32 version number and set the size of the structure
	//		note:	the MSDN documentation about this is a little
	//				dubious and I'm not at all sure if the method
	//				bellow is correct
	ULONGLONG ullVersion = GetDllVersion(_T("Shell32.dll"));
	if(ullVersion >= MAKEDLLVERULL(5, 0,0,0))
		niData.cbSize = sizeof(NOTIFYICONDATA);
	else niData.cbSize = NOTIFYICONDATA_V2_SIZE;

	// the ID number can be anything you choose
	niData.uID = TRAYICONID;

	// state which structure members are valid
	niData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;

	// load the icon
	niData.hIcon = (HICON)LoadImage(hInstance,MAKEINTRESOURCE(IDI_KINECTTRANSPORT),
		IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),
		LR_DEFAULTCOLOR);

	// the window to send messages to and the message to send
	//		note:	the message value should be in the
	//				range of WM_APP through 0xBFFF
	niData.hWnd = hWnd;
    niData.uCallbackMessage = SWM_TRAYMSG;

	// tooltip message
    lstrcpyn(niData.szTip, _T("Kinect Transport\nis running."), sizeof(niData.szTip)/sizeof(TCHAR));

	Shell_NotifyIcon(NIM_ADD,&niData);

	// free icon handle
	if(niData.hIcon && DestroyIcon(niData.hIcon))
		niData.hIcon = NULL;

	// call ShowWindow here to make the dialog initially visible

	// get registry key for settings
	strDestinationHost = "127.0.0.1";
	strConnectedHost = "";
	fSendSkeletonData = true;
	fSendDepthData = false;

	// try to load from registry, if we cannot try to save (in order to create key
	if (!LoadFromRegistry())
		SaveToRegistry();

	return TRUE;
}

BOOL OnInitDialog(HWND hWnd)
{
	HMENU hMenu = GetSystemMenu(hWnd,FALSE);
	if (hMenu)
	{
		AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
		AppendMenu(hMenu, MF_STRING, IDM_ABOUT, _T("About"));
	}
	HICON hIcon = (HICON)LoadImage(hInst,
		MAKEINTRESOURCE(IDI_KINECTTRANSPORT),
		IMAGE_ICON, 0,0, LR_SHARED|LR_DEFAULTSIZE);
	SendMessage(hWnd,WM_SETICON,ICON_BIG,(LPARAM)hIcon);
	SendMessage(hWnd,WM_SETICON,ICON_SMALL,(LPARAM)hIcon);
	return TRUE;
}

// Name says it all
void ShowContextMenu(HWND hWnd)
{
	POINT pt;
	GetCursorPos(&pt);
	HMENU hMenu = CreatePopupMenu();
	if(hMenu)
	{
		InsertMenu(hMenu, -1, MF_BYPOSITION, SWM_SHOW, _T("Options"));
		InsertMenu(hMenu, -1, MF_BYPOSITION, SWM_EXIT, _T("Exit"));

		// note:	must set window to the foreground or the
		//			menu won't disappear when it should
		SetForegroundWindow(hWnd);
		TrackPopupMenu(hMenu, TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL );
		DestroyMenu(hMenu);
	}
}

// Get dll version number
ULONGLONG GetDllVersion(LPCTSTR lpszDllName)
{
    ULONGLONG ullVersion = 0;
	HINSTANCE hinstDll;
    hinstDll = LoadLibrary(lpszDllName);
    if(hinstDll)
    {
        DLLGETVERSIONPROC pDllGetVersion;
        pDllGetVersion = (DLLGETVERSIONPROC)GetProcAddress(hinstDll, "DllGetVersion");
        if(pDllGetVersion)
        {
            DLLVERSIONINFO dvi;
            HRESULT hr;
            ZeroMemory(&dvi, sizeof(dvi));
            dvi.cbSize = sizeof(dvi);
            hr = (*pDllGetVersion)(&dvi);
            if(SUCCEEDED(hr))
				ullVersion = MAKEDLLVERULL(dvi.dwMajorVersion, dvi.dwMinorVersion,0,0);
        }
        FreeLibrary(hinstDll);
    }
    return ullVersion;
}

// Message handler for the app
INT_PTR CALLBACK DlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int wmId, wmEvent;

	switch (message) 
	{
	case SWM_TRAYMSG:
		switch(lParam)
		{
		case WM_LBUTTONDBLCLK:
			ShowWindow(hWnd, SW_RESTORE);
			break;
		case WM_RBUTTONDOWN:
		case WM_CONTEXTMENU:
			ShowContextMenu(hWnd);
		}
		break;
	case WM_SYSCOMMAND:
		if((wParam & 0xFFF0) == SC_MINIMIZE)
		{
			ShowWindow(hWnd, SW_HIDE);
			return 1;
		}
		else if(wParam == IDM_ABOUT)
			DialogBox(hInst, (LPCTSTR)IDD_ABOUTBOX, hWnd, (DLGPROC)About);
		break;
	case WM_COMMAND:
		wmId    = LOWORD(wParam);
		wmEvent = HIWORD(wParam); 

		switch (wmId)
		{
		case SWM_SHOW:
			SetDlgItemText(hWnd, IDC_DESTINATIONHOST, strDestinationHost.c_str());
			CheckDlgButton(hWnd, IDC_SKELETONDATA, fSendSkeletonData ? 1 : 0);
			CheckDlgButton(hWnd, IDC_DEPTHDATA, fSendDepthData ? 1 : 0);
			ShowWindow(hWnd, SW_RESTORE);
			break;
		case IDOK:
			// get values from window and write them to registry
			char destinationHost[100];
			GetDlgItemText(hWnd, IDC_DESTINATIONHOST, destinationHost, 100);
			strDestinationHost = destinationHost;
			fSendSkeletonData = (IsDlgButtonChecked(hWnd, IDC_SKELETONDATA) == 1);
			fSendDepthData = (IsDlgButtonChecked(hWnd, IDC_DEPTHDATA) == 1);
			SaveToRegistry();
			break;
		case SWM_EXIT:
			DestroyWindow(hWnd);
			break;
		case IDM_ABOUT:
			DialogBox(hInst, (LPCTSTR)IDD_ABOUTBOX, hWnd, (DLGPROC)About);
			break;
		}
		return 1;
	case WM_INITDIALOG:
		return OnInitDialog(hWnd);
	case WM_CLOSE:
		ShowWindow(hWnd, SW_HIDE);
		break;
	case WM_DESTROY:
		niData.uFlags = 0;
		Shell_NotifyIcon(NIM_DELETE,&niData);
		PostQuitMessage(0);
		break;
	}
	return 0;
}

// Message handler for about box.
LRESULT CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) 
		{
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	}
	return FALSE;
}

bool UpdateKinect()
{
	if (fSendSkeletonData)
	{
		if (!UpdateKinectSkeleton())
			return false;
	}
	if (fSendDepthData)
	{
		if (!UpdateKinectDepth())
			return false;
	}
	return true;
}

bool UpdateKinectSkeleton()
{
	bool connected = true;

	DWORD dwResult = WaitForSingleObjectEx(reinterpret_cast<HANDLE>(bodyFrameEvent), 0, FALSE);
    if (WAIT_OBJECT_0 != dwResult)
	{
		return connected;
	}

	if (!bodyFrameReader)
	{
        return connected;
	}

    IBodyFrame* pBodyFrame = NULL;
    HRESULT hr = bodyFrameReader->AcquireLatestFrame(&pBodyFrame);

    if (SUCCEEDED(hr))
    {
        INT64 nTime = 0;
        hr = pBodyFrame->get_RelativeTime(&nTime);
        IBody* ppBodies[BODY_COUNT] = {0};

        if (SUCCEEDED(hr))
            hr = pBodyFrame->GetAndRefreshBodyData(_countof(ppBodies), ppBodies);
		
        if (SUCCEEDED(hr))
		{
			// send skeletons as an update
			connected = SendSkeletonUpdate(ppBodies);
		}
		
        for (int i = 0; i < _countof(ppBodies); ++i)
		{
			if (ppBodies[i] != NULL)
				ppBodies[i]->Release();
		}
    }
	
	if (pBodyFrame != NULL)
		pBodyFrame->Release();

	return connected;
}

bool UpdateKinectDepth()
{
	bool connected = true;

	DWORD dwResult = WaitForSingleObjectEx(reinterpret_cast<HANDLE>(depthFrameEvent), 0, FALSE);
    if (WAIT_OBJECT_0 != dwResult)
	{
		return connected;
	}

	if (!bodyFrameReader)
	{
        return connected;
	}

    IDepthFrame* pDepthFrame = NULL;
    HRESULT hr = depthFrameReader->AcquireLatestFrame(&pDepthFrame);

	INT64 nTime = 0;
    IFrameDescription* pFrameDescription = NULL;
    int nWidth = 0;
    int nHeight = 0;
    USHORT nDepthMinReliableDistance = 0;
    USHORT nDepthMaxReliableDistance = 0;
    UINT nBufferSize = 0;
    UINT16 *pBuffer = NULL;

    if (SUCCEEDED(hr))
        hr = pDepthFrame->get_RelativeTime(&nTime);

	if (SUCCEEDED(hr))
		hr = pDepthFrame->get_FrameDescription(&pFrameDescription);

	if (SUCCEEDED(hr))
		hr = pFrameDescription->get_Width(&nWidth);

	if (SUCCEEDED(hr))
		hr = pFrameDescription->get_Height(&nHeight);

	if (SUCCEEDED(hr))
		hr = pDepthFrame->get_DepthMinReliableDistance(&nDepthMinReliableDistance);

	if (SUCCEEDED(hr))
		hr = pDepthFrame->get_DepthMaxReliableDistance(&nDepthMaxReliableDistance);

	if (SUCCEEDED(hr))
		hr = pDepthFrame->AccessUnderlyingBuffer(&nBufferSize, &pBuffer);

	if (SUCCEEDED(hr))
	{
		// send depth as an update
		connected = SendDepthUpdate(nWidth, nHeight, pBuffer, nDepthMinReliableDistance, nDepthMaxReliableDistance);
	}

	if (pDepthFrame != NULL)
		pDepthFrame->Release();

	return connected;
}

bool ConnectToHost(int PortNo, const char* IPAddress)
{
    // Start winsock
    WSADATA wsadata;
    int error = WSAStartup(0x0202, &wsadata);
    if (error)
        return false;

    // Make sure we have winsock v2
    if (wsadata.wVersion != 0x0202)
    {
        WSACleanup();
        return false;
    }

    // Setup socket address
    SOCKADDR_IN target;
    target.sin_family = AF_INET;
    target.sin_port = htons (PortNo);
    target.sin_addr.s_addr = inet_addr(IPAddress);

	// Create socket
    hSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (hSocket == INVALID_SOCKET)
    {
        return false;
    }  

    // Connect
    if (connect(hSocket, (SOCKADDR *)&target, sizeof(target)) == SOCKET_ERROR)
    {
        return false;
    }
    else
        return true;
}

void CloseConnection()
{
    if (hSocket)
        closesocket(hSocket);
    WSACleanup();
}

#pragma pack(push, 1) // exact fit - no padding
struct SkeletonUpdateHeader {
	char command;
	unsigned long dataLength;
	char skeletonsPresent[6];
	unsigned short skeletonCount;
};
#pragma pack(pop)

bool SendSkeletonUpdate(IBody** ppBodies)
{
	if (hSocket)
	{
		// maximum size of frame is 1208 bytes
		char frame[1208];

		SkeletonUpdateHeader header;
		memset(&header, 0, sizeof(SkeletonUpdateHeader));

		// first get skeleton presence
		for (int i = 0; i < BODY_COUNT; ++i)
        {
            IBody* pBody = ppBodies[i];
			if (pBody)
            {
				BOOLEAN bTracked = false;
				if ( SUCCEEDED(pBody->get_IsTracked(&bTracked)) )
				{
					if (bTracked)
					{
						header.skeletonsPresent[i] = 1;
						header.skeletonCount++;
					}
				}
			}
		}

		// write header
		header.command = 0;
		header.dataLength = header.skeletonCount * JointType_Count * 3 * 4 + 8;
		memcpy(frame, &header, sizeof(SkeletonUpdateHeader));
		
		// write skeleton joints
		int byteOffset = sizeof(SkeletonUpdateHeader);
		for (int i = 0; i < 6; ++i)
        {
			// only send skeleton data if this skeleton is present
			if (header.skeletonsPresent[i] == 1)
			{
				Joint joints[JointType_Count]; 
				HRESULT hr = ppBodies[i]->GetJoints(_countof(joints), joints);
				if (SUCCEEDED(hr))
				{
					for (int j = 0; j < _countof(joints); ++j)
					{
						memcpy(&(frame[byteOffset]), &joints[j].Position.X, sizeof(float)); byteOffset += 4;
						memcpy(&(frame[byteOffset]), &joints[j].Position.Y, sizeof(float)); byteOffset += 4;
						memcpy(&(frame[byteOffset]), &joints[j].Position.Z, sizeof(float)); byteOffset += 4;
					}
				}
			}
		}

		// send it out the socket
		int returnVal = send(hSocket, frame, byteOffset, 0);
		if (returnVal == -1)
			return false;
	}
	return true;
}

#pragma pack(push, 1) // exact fit - no padding
struct DepthUpdateHeader {
	char command;
	unsigned long dataLength;
	unsigned short width;
	unsigned short height;
};
#pragma pack(pop)

bool SendDepthUpdate(int nWidth, int nHeight, UINT16 *pBuffer, USHORT nMinDepth, USHORT nMaxDepth)
{
	if (hSocket)
	{
		DepthUpdateHeader header;
		memset(&header, 0, sizeof(DepthUpdateHeader));

		// write header
		header.command = 1;
		header.dataLength = nWidth * nHeight + 4;
		header.width = nWidth;
		header.height = nHeight;
		memcpy(pDepthFrame, &header, sizeof(DepthUpdateHeader));
		
		// write skeleton joints
		int byteOffset = sizeof(DepthUpdateHeader);

        // end pixel is start + width*height - 1
        const UINT16* pBufferEnd = pBuffer + (nWidth * nHeight);

        while (pBuffer < pBufferEnd)
        {
            USHORT depth = *pBuffer;
			if (depth < nMinDepth || depth > nMaxDepth)
				pDepthFrame[byteOffset] = 0;
			else
			{
				//pDepthFrame[byteOffset] = static_cast<char>((depth >= nMinDepth) && (depth <= nMaxDepth) ? (depth % 256) : 0);
				pDepthFrame[byteOffset] = static_cast<char>(((float)depth/(float)nMaxDepth) * 255);
			}
			byteOffset++;
			pBuffer++;
		}

		// send it out the socket
		int returnVal = send(hSocket, pDepthFrame, byteOffset, 0);
		if (returnVal == -1)
			return false;
	}
	return true;
}