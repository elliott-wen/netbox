// NetBox2.cpp : Defines the class behaviors for the application.
//

#include "stdafx.h"

#include "NetBox2.h"
#include "BoxBinPtr.h"

#include "BoxCommand.h"

#include "BoxZipFile.h"
#include "BoxCachePool.h"
#include "BoxScriptObject.h"

#include "boxprotocol.h"
#include <BHook.h>
#include <BBrowserCaps.h>
#include <BFileSystem.h>
#include <BClassRegistry.h>
#include <BProcess.h>
#include <BPipe.h>

#include <mshtmhst.h>
#include <wininet.h>

#include <openssl\rand.h>
#include <openssl\err.h>
#include <openssl\ssl.h>
#include <openssl\bio.h>
#include <openssl\conf.h>

#include <msdasc.h>

#include <lm.h>
#pragma comment( lib, "netapi32.lib" )

static CRITICAL_SECTION *lock_cs;
CComModule _Module;

static void win32_locking_callback(int mode, int type, const char *file, int line)
{
	if (mode & CRYPTO_LOCK)
		EnterCriticalSection(&lock_cs[type]);
	else
		LeaveCriticalSection(&lock_cs[type]);
}

static int win32_add_lock_callback(int *num, int mount, int type, const char *file, int line)
{
	if(mount == 1)
		return InterlockedIncrement((long*)num);
	else if(mount == -1)
		return InterlockedDecrement((long*)num);

	return InterlockedExchangeAdd((long*)num, mount) + mount;
}

static void CRYPTO_thread_setup(void)
{
	int i;

	lock_cs=(CRITICAL_SECTION*)OPENSSL_malloc(CRYPTO_num_locks() * sizeof(CRITICAL_SECTION));
	for (i=0; i<CRYPTO_num_locks(); i++)
		InitializeCriticalSection(&lock_cs[i]);

	CRYPTO_set_locking_callback(win32_locking_callback);
	CRYPTO_set_add_lock_callback(win32_add_lock_callback);
}

static void CRYPTO_thread_cleanup(void)
{
	int i;

	CRYPTO_set_add_lock_callback(NULL);
	CRYPTO_set_locking_callback(NULL);

	for (i=0; i<CRYPTO_num_locks(); i++)
		DeleteCriticalSection(&lock_cs[i]);
	OPENSSL_free(lock_cs);
}

// CNetBox2App construction

CNetBox2App::CNetBox2App() : m_bRunSelfAtExit(FALSE), m_bStep(FALSE), m_nErrorCode(0)
{
#ifdef _DEBUG
	msCheck.Checkpoint();
#endif
/*
	STARTUPINFO StartupInfo;

	memset(&StartupInfo, 0, sizeof(STARTUPINFO));
	StartupInfo.cb = sizeof(STARTUPINFO);
	GetStartupInfo(&StartupInfo);
	strlen(StartupInfo.lpDesktop);

	m_bIsShell = strlen(StartupInfo.lpDesktop)>0;//(::FindWindow(_T("progman"), NULL) != NULL);
*/
	m_bIsShell = TRUE; 

	HWINSTA hWinStation = GetProcessWindowStation();
	if (hWinStation != NULL)
	{
		USEROBJECTFLAGS uof = {0};
		if (GetUserObjectInformation(hWinStation, UOI_FLAGS, &uof, sizeof(USEROBJECTFLAGS), NULL) && ((uof.dwFlags & WSF_VISIBLE) == 0))
			m_bIsShell = FALSE; 
	}

	OSVERSIONINFO  versionInfo;
	BOOL bLowOS = FALSE;
/*
	TCHAR data [4096];
	DWORD dataSize;
	HKEY hKey;
	LONG result;
*/
	::ZeroMemory(&versionInfo, sizeof(OSVERSIONINFO));
	versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

	::GetVersionEx(&versionInfo);

	if(versionInfo.dwMajorVersion < 4)bLowOS = TRUE;
	else if(versionInfo.dwMajorVersion == 4 && versionInfo.dwMinorVersion == 0)
	{
		if(versionInfo.dwPlatformId != VER_PLATFORM_WIN32_NT)
			bLowOS = TRUE;
		else if(!versionInfo.szCSDVersion[0] || (!versionInfo.szCSDVersion[14] && versionInfo.szCSDVersion[13] < '4'))
			bLowOS = TRUE;
	}

/*	if(!bLowOS)
	{
		dataSize = sizeof(data);
		result = ::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
								_T("Software\\Microsoft\\Internet Explorer"),
								0, KEY_QUERY_VALUE, &hKey);

		if(result == ERROR_SUCCESS)
		{
			result = ::RegQueryValueEx(hKey, _T("Version"), NULL, NULL,(LPBYTE) data, &dataSize);

			RegCloseKey(hKey);
		}

		if(result != ERROR_SUCCESS)bLowOS = TRUE;
	}
*/
	if(bLowOS)
	{
		::MessageBox(NULL, "Program cannot run at this machine.", "NetBox Application", 0);
		ExitProcess(0);
	}

	LogEvent(0, "CheckOS");

	EnableAutomation();

	LogEvent(0, "EnableAutomation()");

	m_pService.CreateInstance();

	LogEvent(0, "m_pService.CreateInstance()");

	m_mapMimeType.InitHashTable(127);

	m_hCallProc = CreateEvent(NULL, FALSE, FALSE, NULL);
}

CNetBox2App::~CNetBox2App()
{
	CloseHandle(m_hCallProc);

	if(m_bRunSelfAtExit)
		if(IS_WINNT && m_pArguments->GetCount() == 3 &&
			!m_pArguments->GetString(1).CompareNoCase(L"-Dispatch"))
		{
			m_pService->m_strName = m_pArguments->GetString(2);
			m_pService->Start();
		}else WinExec(GetCommandLine(), SW_SHOW);
}

// The one and only CNetBox2App object

CNetBox2App theApp;


BEGIN_INTERFACE_MAP(CNetBox2App, CWinApp)
	INTERFACE_PART(CNetBox2App, IID_IObjectSafety, ObjectSafety)
END_INTERFACE_MAP()

STDMETHODIMP_(ULONG) CNetBox2App::XObjectSafety::AddRef()
{
	METHOD_PROLOGUE_EX_(CNetBox2App, ObjectSafety)
	return pThis->ExternalAddRef();
}

STDMETHODIMP_(ULONG) CNetBox2App::XObjectSafety::Release()
{
	METHOD_PROLOGUE_EX_(CNetBox2App, ObjectSafety)
	return pThis->ExternalRelease();
}

STDMETHODIMP CNetBox2App::XObjectSafety::QueryInterface(REFIID iid, LPVOID far* ppvObj)
{
	METHOD_PROLOGUE_EX_(CNetBox2App, ObjectSafety)

	return pThis->ExternalQueryInterface(&iid, ppvObj);
}

STDMETHODIMP CNetBox2App::XObjectSafety::GetInterfaceSafetyOptions(REFIID riid, DWORD *pdwSupportedOptions, DWORD *pdwEnabledOptions)
{
	if (!s_bObjectSafety)
		return E_NOINTERFACE;

	if (pdwSupportedOptions == NULL || pdwEnabledOptions == NULL)
		return E_POINTER;

	*pdwSupportedOptions = 3;
	*pdwEnabledOptions   = 1;

	return S_OK;
}

STDMETHODIMP CNetBox2App::XObjectSafety::SetInterfaceSafetyOptions(REFIID riid, DWORD dwOptionSetMask, DWORD dwEnabledOptions)
{
	if (!s_bObjectSafety)
		return E_NOINTERFACE;

	return S_OK;
}

// CNetBox2App initialization

BEGIN_DISPATCH_MAP(CNetBox2App, CWinApp)
	DISP_PROPERTY_EX(CNetBox2App, "Console", get_Console, SetNotSupported, VT_DISPATCH)
	DISP_PROPERTY_EX(CNetBox2App, "Service", get_Service, SetNotSupported, VT_DISPATCH)
	DISP_PROPERTY_EX(CNetBox2App, "Arguments", get_Arguments, SetNotSupported, VT_DISPATCH)

	DISP_FUNCTION(CNetBox2App, "Beep", Beep, VT_EMPTY, VTS_NONE)
	DISP_FUNCTION(CNetBox2App, "Quit", Quit, VT_EMPTY, VTS_I4)
	DISP_FUNCTION(CNetBox2App, "Halt", Halt, VT_EMPTY, VTS_I4)
	DISP_FUNCTION(CNetBox2App, "MsgBox", MsgBox, VT_I4, VTS_BSTR VTS_BSTR VTS_VARIANT)

	DISP_FUNCTION(CNetBox2App, "RegisterServer", RegisterServer, VT_EMPTY, VTS_BSTR)
	DISP_FUNCTION(CNetBox2App, "UnregisterServer", UnregisterServer, VT_EMPTY, VTS_BSTR)

	DISP_FUNCTION(CNetBox2App, "Execute", Execute, VT_I4, VTS_BSTR VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "Exec", Exec, VT_DISPATCH, VTS_BSTR VTS_VARIANT VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "GetAllProcesses", GetAllProcesses, VT_DISPATCH, VTS_NONE)
	DISP_FUNCTION(CNetBox2App, "GetProcess", GetProcess, VT_DISPATCH, VTS_I4 VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "Shutdown", Shutdown, VT_EMPTY, VTS_BOOL)

	DISP_FUNCTION(CNetBox2App, "SendMessage", SendMessage, VT_EMPTY, VTS_BSTR VTS_BSTR)

	DISP_FUNCTION(CNetBox2App, "Command", Command, VT_EMPTY, VTS_NONE)

	DISP_FUNCTION(CNetBox2App, "AppActivate", AppActivate, VT_EMPTY, VTS_BSTR)

	DISP_FUNCTION(CNetBox2App, "OpenFileDialog", OpenFileDialog, VT_BSTR, VTS_VARIANT VTS_VARIANT VTS_VARIANT VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "SaveFileDialog", SaveFileDialog, VT_BSTR, VTS_VARIANT VTS_VARIANT VTS_VARIANT VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "BrowseForFolder", BrowseForFolder, VT_BSTR, VTS_VARIANT VTS_VARIANT VTS_VARIANT VTS_VARIANT)

	DISP_FUNCTION(CNetBox2App, "DoEvents", DoEvents, VT_EMPTY, VTS_NONE)

	DISP_FUNCTION(CNetBox2App, "LoadPrivateKey", LoadPrivateKey, VT_I4, VTS_BSTR VTS_BSTR)

	DISP_FUNCTION(CNetBox2App, "GetThreadId", GetThreadId, VT_I4, VTS_NONE)

	DISP_FUNCTION(CNetBox2App, "FindWindow", FindWindow, VT_I4, VTS_BSTR VTS_BSTR)
	DISP_FUNCTION(CNetBox2App, "FindWindowEx", FindWindowEx, VT_I4, VTS_I4 VTS_I4 VTS_BSTR VTS_BSTR)
	DISP_FUNCTION(CNetBox2App, "EnumWindows", EnumWindows, VT_DISPATCH, VTS_I4)
	DISP_FUNCTION(CNetBox2App, "GetWindowText", GetWindowText, VT_BSTR, VTS_I4)
	DISP_FUNCTION(CNetBox2App, "SetWindowText", SetWindowText, VT_I4, VTS_I4 VTS_BSTR)
	DISP_FUNCTION(CNetBox2App, "SetForegroundWindow", SetForegroundWindow, VT_I4, VTS_I4)
	DISP_FUNCTION(CNetBox2App, "ShowWindow", ShowWindow, VT_I4, VTS_I4 VTS_I4)
	DISP_FUNCTION(CNetBox2App, "PostMessage", PostMessage, VT_I4, VTS_I4 VTS_I4 VTS_I4 VTS_I4)
	DISP_FUNCTION(CNetBox2App, "GetDlgCtrlID", GetDlgCtrlID, VT_I4, VTS_I4)
	DISP_FUNCTION(CNetBox2App, "GetDlgItem", GetDlgItem, VT_I4, VTS_I4 VTS_I4)
	DISP_FUNCTION(CNetBox2App, "GetWindowProcessId", GetWindowProcessId, VT_I4, VTS_I4)

	DISP_FUNCTION(CNetBox2App, "RegRead", RegRead, VT_VARIANT, VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "RegWrite", RegWrite, VT_EMPTY, VTS_VARIANT VTS_VARIANT VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "RegDelete", RegDelete, VT_EMPTY, VTS_VARIANT)

	DISP_FUNCTION(CNetBox2App, "RegEnumKey", RegEnumKey, VT_VARIANT, VTS_VARIANT)
	DISP_FUNCTION(CNetBox2App, "RegEnumValue", RegEnumValue, VT_VARIANT, VTS_VARIANT)

	DISP_FUNCTION(CNetBox2App, "EnableWow64FsRedirection", EnableWow64FsRedirection, VT_I4, VTS_I4)

	DISP_PROPERTY_EX(CNetBox2App, "StdIn", get_StdIn, SetNotSupported, VT_DISPATCH)
	DISP_PROPERTY_EX(CNetBox2App, "StdOut", get_StdOut, SetNotSupported, VT_DISPATCH)
	DISP_PROPERTY_EX(CNetBox2App, "StdErr", get_StdErr, SetNotSupported, VT_DISPATCH)
END_DISPATCH_MAP()


typedef BOOLEAN (WINAPI *LPFN_Wow64EnableWow64FsRedirection) (BOOLEAN);

LPFN_Wow64EnableWow64FsRedirection 
fnWow64EnableWow64FsRedirection = (LPFN_Wow64EnableWow64FsRedirection)GetProcAddress(
GetModuleHandle("kernel32"),"Wow64EnableWow64FsRedirection");

long CNetBox2App::EnableWow64FsRedirection(long lEnabled)
{
	return fnWow64EnableWow64FsRedirection?fnWow64EnableWow64FsRedirection(lEnabled?TRUE:FALSE):FALSE;
}

static struct
{
	LPCWSTR	Name;
	HKEY	hKey;
} s_HKeys[]= 
{
	{
		L"HKEY_CURRENT_USER",
		HKEY_CURRENT_USER
	},
	{
		L"HKCU",
		HKEY_CURRENT_USER
	},
	{
		L"HKEY_LOCAL_MACHINE",
		HKEY_LOCAL_MACHINE
	},
	{
		L"HKLM",
		HKEY_LOCAL_MACHINE
	},
	{
		L"HKEY_CLASSES_ROOT",
		HKEY_CLASSES_ROOT
	},
	{
		L"HKCR",
		HKEY_CLASSES_ROOT
	},
	{
		L"HKEY_USERS",
		HKEY_USERS
	},
	{
		L"HKEY_CURRENT_CONFIG",
		HKEY_CURRENT_CONFIG
	},
	{
		L"HKEY_DYN_DATA",
		HKEY_DYN_DATA
	},
	{
		L"HKEY_PERFORMANCE_DATA",
		HKEY_PERFORMANCE_DATA
	},
	{
		L"HKEY_PERFORMANCE_TEXT",
		HKEY_PERFORMANCE_TEXT
	},
	{
		L"HKEY_PERFORMANCE_NLSTEXT",
		HKEY_PERFORMANCE_NLSTEXT
	}
};

static struct
{
	LPCWSTR	Name;
	int		Type;
} s_RegTypes[]= 
{
	{
		L"REG_SZ",
		REG_SZ
	},
	{
		L"REG_EXPAND_SZ",
		REG_EXPAND_SZ
	},
	{
		L"REG_BINARY",
		REG_BINARY
	},
	{
		L"REG_DWORD",
		REG_DWORD
	},
	{
		L"REG_MULTI_SZ",
		REG_MULTI_SZ
	}
};

HRESULT CNetBox2App::RegSplitKey(VARIANT& varKey, HKEY* phKey, CBString& strKey, CBString& strValue, int *pIs64KEY)
{
	while(varKey.vt == (VT_VARIANT | VT_BYREF))
		varKey = *varKey.pvarVal;

	if (varKey.vt != VT_BSTR)
		return E_INVALIDARG;

	CStringW strPath(varKey), strHKey;

	int p0, p1;
	p0 = strPath.Find('\\');
	if (p0 == -1)
		AfxThrowOleException(E_INVALIDARG);
	
	strHKey = strPath.Left(p0);
	if (strHKey.Find(L"Win64::") == 0)
	{
		*pIs64KEY = 1;
		strHKey = strHKey.Mid(7);
	}
	else
		*pIs64KEY = 0;

	int i;
	for(i = 0; i < sizeof(s_HKeys) / sizeof(s_HKeys[0]); i ++)
		if(!_wcsicmp(s_HKeys[i].Name, strHKey))
			break;
	if (i == sizeof(s_HKeys) / sizeof(s_HKeys[0]))
		return E_INVALIDARG;

	*phKey = s_HKeys[i].hKey;

	p1 = strPath.ReverseFind('\\');
	if (p1 == -1)
		AfxThrowOleException(E_INVALIDARG);

	if (p0 == p1)
		strKey = L"";
	else
		strKey = strPath.Mid(p0+1, p1 - p0 - 1);

	if (strPath.GetLength()-1 == p1)
		strValue = L"";
	else
		strValue = strPath.Mid(p1 + 1);

	return S_OK;
}

void CNetBox2App::RegWrite(VARIANT& varKey, VARIANT& varValue, VARIANT& varType)
{
	HKEY hKey;
	CBString strKey, strValue;
	HRESULT hr;
	int Is64Key = 0;

	hr = RegSplitKey(varKey, &hKey, strKey, strValue, &Is64Key);
	if (FAILED(hr))
		AfxThrowOleException(hr);

	while(varType.vt == (VT_VARIANT | VT_BYREF))
		varType = *varType.pvarVal;

	int iAutoType = 1;
	if(varType.vt != VT_ERROR)
	{
		if (varType.vt != VT_BSTR)
			AfxThrowOleException(E_INVALIDARG);
		iAutoType = 0;
	}

	while(varValue.vt == (VT_VARIANT | VT_BYREF))
		varValue = *varValue.pvarVal;

	CComVariant value;
	VARIANT *pvarValue;
	if(varValue.vt == VT_UNKNOWN || varValue.vt == VT_DISPATCH)
	{
		CComDispatchDriver pdisp = varValue.pdispVal;
		if (pdisp == NULL)
			AfxThrowOleException(E_INVALIDARG);
		pdisp.Invoke0((DISPID)0, &value);
		pvarValue = &value;
	}
	else
	{
		pvarValue = &varValue;
	}

	if (iAutoType == 0)
	{
		for(iAutoType = 0; iAutoType < sizeof(s_RegTypes) / sizeof(s_RegTypes[0]); iAutoType ++)
			if(!_wcsicmp(s_RegTypes[iAutoType].Name, varType.bstrVal))
				break;
		if (iAutoType == sizeof(s_RegTypes) / sizeof(s_RegTypes[0]))
			AfxThrowOleException(E_INVALIDARG);
		iAutoType = s_RegTypes[iAutoType].Type;
	}
	else
	{
		
		switch (pvarValue->vt)
		{
			case VT_I1:
			case VT_UI1:
			case VT_I2:
			case VT_UI2:
			case VT_I4:
			case VT_UI4:
			case VT_BOOL:
				iAutoType = REG_DWORD;
				break;
			case VT_BSTR:
				iAutoType = REG_SZ;
				break;
			case VT_UI1|VT_ARRAY:
				iAutoType = REG_BINARY;
				break;
			case VT_VARIANT|VT_ARRAY|VT_BYREF:
			case VT_VARIANT|VT_ARRAY:
				iAutoType = REG_MULTI_SZ;
				break;
			default:
				iAutoType = REG_SZ;
		}
	}

	CComVariant var;
	DWORD dwRet = RegOpenKeyExW(hKey, L"", 0, KEY_READ|(Is64Key?KEY_WOW64_64KEY:0), &hKey);
	if (dwRet != ERROR_SUCCESS)
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
	switch (iAutoType)
	{
		case REG_DWORD:
			var.ChangeType(VT_UI4, pvarValue);
			dwRet = SHSetValueW(hKey, strKey, strValue, iAutoType, &var.ulVal, 4);
			break;
		case REG_SZ:
		case REG_EXPAND_SZ:
			if (pvarValue->vt != VT_BSTR)
			{
				var.ChangeType(VT_BSTR, pvarValue);
				pvarValue = &var;
			}
			dwRet = SHSetValueW(hKey, strKey, strValue, iAutoType, (LPBYTE)pvarValue->bstrVal, SysStringByteLen(pvarValue->bstrVal)+2);
			break;
		case REG_BINARY:
			{
				CBoxBinPtr varPtr(*pvarValue);
				dwRet = SHSetValueW(hKey, strKey, strValue, iAutoType, varPtr.m_pData, varPtr.m_nSize);
			}
			break;
		case REG_MULTI_SZ:
			{
				if (pvarValue->vt == (VT_VARIANT|VT_ARRAY) || pvarValue->vt == (VT_VARIANT|VT_ARRAY|VT_BYREF))
				{
					int i, nSize = 0;
					VARIANT *pv;
					CComSafeArray<VARIANT> sa;
					
					sa.Attach(pvarValue->vt&VT_BYREF?*pvarValue->pparray:pvarValue->parray);
					if (sa.GetDimensions() != 1)
					{
						sa.Detach();
						RegCloseKey(hKey);
						AfxThrowOleException(E_INVALIDARG);
					}
					CAtlArray<CComVariant> arr_var;
					CAtlArray<BSTR> arr_bstr;
					arr_var.SetCount(sa.GetCount());
					arr_bstr.SetCount(sa.GetCount());
					for (i = 0;i < sa.GetCount(); i ++)
					{
						pv = &sa[i];
						if (pv->vt == VT_BSTR)
							arr_bstr[i] = pv->bstrVal;
						else
						{
							arr_var[i].ChangeType(VT_BSTR, pv);
							arr_bstr[i] = arr_var[i].bstrVal;
						}
						nSize += SysStringLen(arr_bstr[i]) + 1;
					}
					sa.Detach();
					nSize++;

					CAutoPtr<WCHAR> pwstr;
					pwstr.Attach(new WCHAR[nSize]);
					LPWSTR p = pwstr;
					for (i = 0;i < arr_bstr.GetCount(); i ++)
					{
						DWORD n = SysStringLen(arr_bstr[i]);
						memcpy(p, arr_bstr[i], n*2);
						p[n] = 0;
						p+=n+1;
					}
					*p = 0;

					dwRet = SHSetValueW(hKey, strKey, strValue, iAutoType, (LPBYTE)(void *)pwstr, nSize*2);
				}
				else
				{
					if (pvarValue->vt != VT_BSTR)
					{
						var.ChangeType(VT_BSTR, pvarValue);
						pvarValue = &var;
					}

					int nSize = SysStringLen(pvarValue->bstrVal)+2;
					CAutoPtr<WCHAR> pwstr;
					pwstr.Attach(new WCHAR[nSize]);
					memcpy(pwstr, pvarValue->bstrVal, (nSize-2)*2);
					pwstr[nSize-2] = 0;
					pwstr[nSize-1] = 0;
					
					dwRet = SHSetValueW(hKey, strKey, strValue, iAutoType, (LPBYTE)(void *)pwstr, nSize*2);
				}
			}
			break;
	}
	RegCloseKey(hKey);
	if (dwRet != ERROR_SUCCESS)
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
}

void CNetBox2App::RegDelete(VARIANT& varKey)
{
	HKEY hKey;
	CBString strKey, strValue;
	HRESULT hr;
	int Is64Key = 0;

	hr = RegSplitKey(varKey, &hKey, strKey, strValue, &Is64Key);
	if (FAILED(hr))
		AfxThrowOleException(hr);
	
	DWORD dwRet = RegOpenKeyExW(hKey, L"", 0, KEY_WRITE|(Is64Key?KEY_WOW64_64KEY:0), &hKey);
	if (dwRet != ERROR_SUCCESS)
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
	if (strValue.GetLength() == 0)
		dwRet = SHDeleteKeyW(hKey, strKey);
	else
		dwRet = SHDeleteValueW(hKey, strKey, strValue);
	RegCloseKey(hKey);

	if (dwRet != ERROR_SUCCESS)
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
}

VARIANT CNetBox2App::RegRead(VARIANT& varKey)
{
	HKEY hKey;
	CBString strKey, strValue;
	HRESULT hr;
	int Is64Key = 0;

	hr = RegSplitKey(varKey, &hKey, strKey, strValue, &Is64Key);
	if (FAILED(hr))
		AfxThrowOleException(hr);

	DWORD dwType = 0, dwSize = 0, dwRet;
	
	dwRet = RegOpenKeyExW(hKey, strKey, 0, KEY_READ|(Is64Key?KEY_WOW64_64KEY:0), &hKey);
	if (dwRet != ERROR_SUCCESS)
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));

	dwRet = RegQueryValueExW(hKey, strValue, NULL, &dwType, NULL, &dwSize);
	if (dwRet != ERROR_SUCCESS)
	{
		RegCloseKey(hKey);
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
	}

	switch(dwType)
	{
		case REG_BINARY:
		{
			CBoxBinPtr varPtr(dwSize);
			dwRet = RegQueryValueExW(hKey, strValue, NULL, &dwType, varPtr, &dwSize);
			RegCloseKey(hKey);
			if (dwRet != ERROR_SUCCESS)
				AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
			return varPtr.GetVariant();
		}
		case REG_DWORD:
		{
			VARIANT varRet;
			::VariantInit(&varRet);
			varRet.vt = VT_I4;
			dwRet = RegQueryValueExW(hKey, strValue, NULL, &dwType, (LPBYTE)&varRet.lVal, &dwSize);
			RegCloseKey(hKey);
			if (dwRet != ERROR_SUCCESS)
				AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
			return varRet;
		}
		case REG_MULTI_SZ:
		{
			CAutoPtr<WCHAR> wstr((WCHAR *)new BYTE[dwSize]);
			LPWSTR wstr1, wstr2, estr;
			dwRet = RegQueryValueExW(hKey, strValue, NULL, &dwType, (LPBYTE)(void *)wstr, &dwSize);
			RegCloseKey(hKey);
			if (dwRet != ERROR_SUCCESS)
				AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));

			CAtlArray<BSTR> arrayStr;
			wstr2 = wstr1 = wstr;
			estr = (LPWSTR)((LPBYTE)(void *)wstr + dwSize);
			while (wstr2<estr)
			{
				while (*wstr2)
					wstr2++;
				wstr2++;
				if (wstr2<estr || wstr2-wstr1-1>0)
					arrayStr.Add(SysAllocString(wstr1));
				wstr1 = wstr2;
			}
			if (wstr2-wstr1)
				arrayStr.Add(SysAllocStringLen(wstr1, wstr2-wstr1));

			CComSafeArray<VARIANT> sa((ULONG)arrayStr.GetCount());
			for (int i=0;i<arrayStr.GetCount();i++)
			{
				(&sa[i])->vt = VT_BSTR;
				(&sa[i])->bstrVal = arrayStr[i];
			}
			VARIANT var;
			::VariantInit(&var);
			var.vt = VT_ARRAY | VT_VARIANT;
			var.parray = sa.Detach();
			return var;
		}
		case REG_EXPAND_SZ:
		case REG_SZ:
		{
			CComBSTR bstr;
			if (dwSize > 2)
			{
				bstr.Attach(SysAllocStringByteLen(NULL, dwSize-2));
				dwRet = RegQueryValueExW(hKey, strValue, NULL, &dwType, (LPBYTE)(void *)bstr, &dwSize);
				RegCloseKey(hKey);
				if (dwRet != ERROR_SUCCESS)
					AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
				*((WCHAR *)((char *)bstr.m_str+dwSize-2)) = 0;
			}
			else
				bstr.Attach(SysAllocStringByteLen(NULL, 0));
			VARIANT varRet;
			::VariantInit(&varRet);
			varRet.vt = VT_BSTR;
			varRet.bstrVal = bstr.Detach();;
			return varRet;
		}
	}
	RegCloseKey(hKey);
	AfxThrowOleException(E_NOTIMPL);
}

#define MAX_KEY_LENGTH 255
#define MAX_VALUE_NAME 16383

VARIANT CNetBox2App::RegEnumValue(VARIANT& varKey)
{
	HKEY hKey;
	CBString strKey, strValue;
	HRESULT hr;
	int Is64Key = 0;

	hr = RegSplitKey(varKey, &hKey, strKey, strValue, &Is64Key);
	if (FAILED(hr))
		AfxThrowOleException(hr);

	if (strValue.GetLength()>0)
		AfxThrowOleException(E_INVALIDARG);
	
	DWORD dwRet, i;

	dwRet = RegOpenKeyExW(hKey, strKey, 0, KEY_READ|(Is64Key?KEY_WOW64_64KEY:0), &hKey);
	if (dwRet != ERROR_SUCCESS)
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));

    WCHAR    achClass[MAX_PATH] = L"";  // buffer for class name 
    DWORD    cchClassName = MAX_PATH;  // size of class string 
    DWORD    cSubKeys=0;               // number of subkeys 
    DWORD    cbMaxSubKey;              // longest subkey size 
    DWORD    cchMaxClass;              // longest class string 
    DWORD    cValues;              // number of values for key 
    DWORD    cchMaxValue;          // longest value name 
    DWORD    cbMaxValueData;       // longest value data 
    DWORD    cbSecurityDescriptor; // size of security descriptor 
    FILETIME ftLastWriteTime;      // last write time 
 
    // Get the class name and the value count. 
    dwRet = RegQueryInfoKeyW(
        hKey,                    // key handle 
        achClass,                // buffer for class name 
        &cchClassName,           // size of class string 
        NULL,                    // reserved 
        &cSubKeys,               // number of subkeys 
        &cbMaxSubKey,            // longest subkey size 
        &cchMaxClass,            // longest class string 
        &cValues,                // number of values for this key 
        &cchMaxValue,            // longest value name 
        &cbMaxValueData,         // longest value data 
        &cbSecurityDescriptor,   // security descriptor 
        &ftLastWriteTime);       // last write time 
	if (dwRet != ERROR_SUCCESS)
	{
		RegCloseKey(hKey);
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
	}

    WCHAR achValue[MAX_VALUE_NAME];
    DWORD cchValue = MAX_VALUE_NAME;

	CAtlArray<BSTR> arrayStr;
    if (cValues)
    {
        for (i=0; i<cValues; i++) 
        { 
            cchValue = MAX_VALUE_NAME;
			achValue[0] = 0; 
            dwRet = RegEnumValueW(hKey, i, achValue, &cchValue, NULL, NULL, NULL, NULL);
            if (dwRet != ERROR_SUCCESS)
                break;
			arrayStr.Add(SysAllocString(achValue));
        }
    } 
	RegCloseKey(hKey);

	CComSafeArray<VARIANT> sa((ULONG)arrayStr.GetCount());
	for (int i=0;i<arrayStr.GetCount();i++)
	{
		(&sa[i])->vt = VT_BSTR;
		(&sa[i])->bstrVal = arrayStr[i];
	}
	VARIANT var;
	::VariantInit(&var);
	var.vt = VT_ARRAY | VT_VARIANT;
	var.parray = sa.Detach();
	return var;
}

VARIANT CNetBox2App::RegEnumKey(VARIANT& varKey)
{
	HKEY hKey;
	CBString strKey, strValue;
	HRESULT hr;
	int Is64Key = 0;

	hr = RegSplitKey(varKey, &hKey, strKey, strValue, &Is64Key);
	if (FAILED(hr))
		AfxThrowOleException(hr);

	if (strValue.GetLength()>0)
		AfxThrowOleException(E_INVALIDARG);
	
	DWORD dwRet, i;

	dwRet = RegOpenKeyExW(hKey, strKey, 0, KEY_READ|(Is64Key?KEY_WOW64_64KEY:0), &hKey);
	if (dwRet != ERROR_SUCCESS)
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));

    WCHAR    achKey[MAX_KEY_LENGTH];   // buffer for subkey name
    DWORD    cbName;                   // size of name string 
    WCHAR    achClass[MAX_PATH] = L"";  // buffer for class name 
    DWORD    cchClassName = MAX_PATH;  // size of class string 
    DWORD    cSubKeys=0;               // number of subkeys 
    DWORD    cbMaxSubKey;              // longest subkey size 
    DWORD    cchMaxClass;              // longest class string 
    DWORD    cValues;              // number of values for key 
    DWORD    cchMaxValue;          // longest value name 
    DWORD    cbMaxValueData;       // longest value data 
    DWORD    cbSecurityDescriptor; // size of security descriptor 
    FILETIME ftLastWriteTime;      // last write time 
 
    // Get the class name and the value count. 
    dwRet = RegQueryInfoKeyW(
        hKey,                    // key handle 
        achClass,                // buffer for class name 
        &cchClassName,           // size of class string 
        NULL,                    // reserved 
        &cSubKeys,               // number of subkeys 
        &cbMaxSubKey,            // longest subkey size 
        &cchMaxClass,            // longest class string 
        &cValues,                // number of values for this key 
        &cchMaxValue,            // longest value name 
        &cbMaxValueData,         // longest value data 
        &cbSecurityDescriptor,   // security descriptor 
        &ftLastWriteTime);       // last write time 
	if (dwRet != ERROR_SUCCESS)
	{
		RegCloseKey(hKey);
		AfxThrowOleException(HRESULT_FROM_WIN32(dwRet));
	}

	CAtlArray<BSTR> arrayStr;
    if (cSubKeys)
    {
        for (i=0; i<cSubKeys; i++) 
        { 
            cbName = MAX_KEY_LENGTH;
            dwRet = RegEnumKeyExW(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime); 
            if (dwRet != ERROR_SUCCESS)
                break;
			arrayStr.Add(SysAllocString(achKey));
        }
    } 
	RegCloseKey(hKey);

	CComSafeArray<VARIANT> sa((ULONG)arrayStr.GetCount());
	for (int i=0;i<arrayStr.GetCount();i++)
	{
		(&sa[i])->vt = VT_BSTR;
		(&sa[i])->bstrVal = arrayStr[i];
	}
	VARIANT var;
	::VariantInit(&var);
	var.vt = VT_ARRAY | VT_VARIANT;
	var.parray = sa.Detach();
	return var;
}

long CNetBox2App::FindWindow(LPCTSTR lpcsClass, LPCTSTR lpcsTitle)
{
	return (long)::FindWindow((lpcsClass && _tcslen(lpcsClass))?lpcsClass:NULL, (lpcsTitle && _tcslen(lpcsTitle))?lpcsTitle:NULL);
}

long CNetBox2App::FindWindowEx(long hwndParent, long hwndChildAfter, LPCTSTR lpcsClass, LPCTSTR lpcsTitle)
{
	return (long)::FindWindowEx((HWND)hwndParent, (HWND)hwndChildAfter, (lpcsClass && _tcslen(lpcsClass))?lpcsClass:NULL, (lpcsTitle && _tcslen(lpcsTitle))?lpcsTitle:NULL);
}

BOOL CALLBACK EnumWindowProc(HWND hWnd, LPVOID lpContext)
{
	TCHAR buf[65536];
	::SendMessage((HWND)hWnd, WM_GETTEXT, sizeof(buf), (LPARAM)buf);
	CComVariant varKey((LONG)hWnd), varValue(buf);
	((CBDictionary*)lpContext)->put_Item(varKey, varValue);
	return TRUE;
}

LPDISPATCH CNetBox2App::EnumWindows(long hWnd)
{
	CBComPtr<CBDictionary> pdic;
	HRESULT hr = pdic.CreateInstance();
	if (FAILED(hr)) AfxThrowOleException(hr);
	CComVariant var(FALSE);
	pdic->put_ArrayMode(VARIANT_FALSE);
	if (hWnd)
		::EnumChildWindows((HWND)hWnd, (WNDENUMPROC)EnumWindowProc, (LPARAM)(LPVOID)pdic);
	else
		::EnumWindows((WNDENUMPROC)EnumWindowProc, (LPARAM)(LPVOID)pdic);
	return pdic.Detach();
}

BSTR CNetBox2App::GetWindowText(long hWnd)
{
	TCHAR buf[65536];
	DWORD n = ::SendMessage((HWND)hWnd, WM_GETTEXT, sizeof(buf), (LPARAM)buf);
	buf[n] = 0;
	CComBSTR bstr(buf);
	return bstr.Detach();
}

long CNetBox2App::SetWindowText(LONG hWnd, LPCTSTR pstrTitle)
{
	return (long)::SendMessage((HWND)hWnd, WM_SETTEXT, NULL, (LPARAM)pstrTitle);
}

long CNetBox2App::SetForegroundWindow(LONG hWnd)
{
	return (long)::SetForegroundWindow((HWND)hWnd);
}

long CNetBox2App::ShowWindow(LONG hWnd, long nCmdShow)
{
	return (long)::ShowWindow((HWND)hWnd, nCmdShow);
}

long CNetBox2App::PostMessage(long hWnd, long uMsg, long wParam, long lParam)
{
	return ::PostMessage((HWND)hWnd, (UINT)uMsg, (WPARAM)wParam, (LPARAM)lParam);
}

long CNetBox2App::GetDlgCtrlID(long hWnd)
{
	return ::GetDlgCtrlID((HWND)hWnd);
}

long CNetBox2App::GetDlgItem(long hDlg, long DlgCtrlID)
{
	return (long)::GetDlgItem((HWND)hDlg, DlgCtrlID);
}

long CNetBox2App::GetWindowProcessId(long hWnd)
{
	DWORD uiProcessId;
	GetWindowThreadProcessId((HWND)hWnd, (LPDWORD)&uiProcessId);
	return uiProcessId;
}

LPDISPATCH CNetBox2App::get_StdIn()
{
	if(!m_pStdIn)
	{
		HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
		if (h == INVALID_HANDLE_VALUE)
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
		if (h == NULL)
			AfxThrowOleException(HRESULT_FROM_WIN32(E_ACCESSDENIED));

		m_pStdIn.CreateInstance();
		m_pStdIn->SetHandle(h);
	}

	CBDispatchPtr pDisp(m_pStdIn);

	return pDisp.Detach();
}

LPDISPATCH CNetBox2App::get_StdOut()
{
	if(!m_pStdOut)
	{
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		if (h == INVALID_HANDLE_VALUE)
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
		if (h == NULL)
			AfxThrowOleException(HRESULT_FROM_WIN32(E_ACCESSDENIED));

		m_pStdOut.CreateInstance();
		m_pStdOut->SetHandle(h);
	}

	CBDispatchPtr pDisp(m_pStdOut);

	return pDisp.Detach();
}

LPDISPATCH CNetBox2App::get_StdErr()
{
	if(!m_pStdErr)
	{
		HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
		if (h == INVALID_HANDLE_VALUE)
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
		if (h == NULL)
			AfxThrowOleException(HRESULT_FROM_WIN32(E_ACCESSDENIED));

		m_pStdErr.CreateInstance();
		m_pStdErr->SetHandle(h);
	}

	CBDispatchPtr pDisp(m_pStdErr);

	return pDisp.Detach();
}

LPDISPATCH CNetBox2App::get_Console()
{
	if(!m_pConsole)
	{
		/*
		BOOL (WINAPI *pAttachConsole)(DWORD dwProcessId);

		*(FARPROC*)&pAttachConsole = ::GetProcAddress(GetModuleHandle(_T("Kernel32.dll")), _T("AttachConsole"));
		
		if (!pAttachConsole(-1))		//Windows NT >=5.0, XP 2003
*/			::AllocConsole();
		::SetConsoleTitle(CBoxSystem::getVersion());
		m_pConsole.CreateInstance();
	}

	CBDispatchPtr pDisp(m_pConsole);

	return pDisp.Detach();
}

LPDISPATCH CNetBox2App::get_Service(void)
{
	m_pService->ExternalAddRef();
	return m_pService;
}

LPDISPATCH CNetBox2App::get_Arguments(void)
{
	CBComPtr<IDispatch> pDisp = m_pArguments;

	return pDisp.Detach();
}

void CNetBox2App::Beep()
{
	::MessageBeep(-1);
}

void CNetBox2App::AppActivate(LPCTSTR pstrTitle)
{
	HWND hWnd = ::FindWindow(NULL, pstrTitle);

	if(hWnd == NULL)
		AfxThrowOleDispatchException(0x4005, _T("Application not found : ") + CString(pstrTitle), 0);

	::ShowWindow(hWnd, SW_SHOWNORMAL);
	::SetForegroundWindow(hWnd);
}

void CNetBox2App::Quit(long nErrorCode)
{
	CScriptHost* pNowScript = CScriptHost::GetCurrentScript();

	if(pNowScript != NULL)
		pNowScript->Stop(nErrorCode);
}

long CNetBox2App::GetThreadId()
{
	return (long)GetCurrentThreadId();
}

void CNetBox2App::Halt(long nErrorCode)
{
	m_nErrorCode = nErrorCode | 0x80000000;

	m_pService->Halt();
	::SuspendThread(GetCurrentThread());
}

long CNetBox2App::MsgBox(LPCTSTR pstrText, LPCTSTR pstrTitle, VARIANT* varType)
{
	UINT uType = 0;

	if(varType->vt != VT_ERROR)
		uType = varGetNumber(varType);

	if(m_bIsShell)return ::MessageBox(::GetForegroundWindow(), pstrText, pstrTitle, uType);

	return 0;
}

void CNetBox2App::SendMessage(LPCTSTR pstrSendTo, LPCTSTR pstrMessage)
{
	if(IS_WINNT)
	{
		CStringW strTemp;
		strTemp = pstrMessage;
		NetMessageBufferSend( NULL, CStringW(pstrSendTo), NULL, (byte *)(LPCWSTR)strTemp, strTemp.GetLength() * 2);
	}else
	{
		CString strMailSlot, strMessage;

		HANDLE hHandle;
		DWORD dwBytesWritten;

		strMailSlot.Format(_T("\\\\%s\\MAILSLOT\\messngr"), pstrSendTo);

		dwBytesWritten = MAX_COMPUTERNAME_LENGTH + 1;
		GetComputerName(strMessage.GetBuffer(MAX_COMPUTERNAME_LENGTH + 1), &dwBytesWritten);
		strMessage.ReleaseBuffer(dwBytesWritten);

		strMessage.AppendChar(0);
		strMessage.Append(pstrSendTo);
		strMessage.AppendChar(0);
		strMessage.Append(pstrMessage);
		strMessage.AppendChar(0);

		hHandle = CreateFile(strMailSlot, GENERIC_WRITE, FILE_SHARE_READ, NULL,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hHandle)
		{
			WriteFile(hHandle, strMessage, (DWORD)strMessage.GetLength(), &dwBytesWritten, NULL);
			CloseHandle(hHandle);
		}
	}
}

void CNetBox2App::RegisterServer(LPCTSTR pstrName)
{
	HRESULT hr = CBClassRegistry::RegSvr32(CBString(pstrName), 1);
	if(FAILED(hr))AfxThrowOleException(hr);
}

void CNetBox2App::UnregisterServer(LPCTSTR pstrName)
{
	HRESULT hr = CBClassRegistry::RegSvr32(CBString(pstrName), 0);
	if(FAILED(hr))AfxThrowOleException(hr);
}

LPDISPATCH CNetBox2App::GetAllProcesses()
{
	//PROCESS_QUERY_INFORMATION|PROCESS_VM_READ
    DWORD aProcesses[1024], cbNeeded, cProcesses;

	BOOL (__stdcall *EnumProcesses)(DWORD*, DWORD, DWORD*) = NULL;
	HMODULE hmod = ::NewLoadLibraryA("Psapi.dll");
	if(hmod == NULL)
		AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));

	EnumProcesses = (BOOL (__stdcall *)(DWORD*, DWORD, DWORD*))GetProcAddress(hmod, "EnumProcesses");
	if(EnumProcesses == NULL)
	{
		FreeLibrary(hmod);
		AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
	}

	if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded))
	{
		FreeLibrary(hmod);
		AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
	}
	FreeLibrary(hmod);

    CBComPtr<CBListEx> pRet;
	pRet.CreateInstance();
	cProcesses = cbNeeded / sizeof(DWORD);
    for (DWORD i=0;i<cProcesses;i++ )
	{
		if(aProcesses[i]!=0)
		{
			CComVariant Key;
			Key = (LONG)aProcesses[i];
			pRet->Add(Key);
		}
	}
	return (IVariantStruct *)pRet.Detach();
}

LPDISPATCH CNetBox2App::GetProcess(LONG lProcessID, VARIANT* varRights)
{
	DWORD lRights = 7;
	if(varRights->vt != VT_ERROR)
		lRights = varGetNumber(varRights);

	DWORD dwRight = SYNCHRONIZE;
	if (lRights & 1) dwRight |= PROCESS_QUERY_INFORMATION;
	if (lRights & 2) dwRight |= PROCESS_VM_READ;
	if (lRights & 4) dwRight |= PROCESS_TERMINATE;

	HANDLE hProcess = ::OpenProcess(dwRight, FALSE, lProcessID);
	if(hProcess == NULL)
	{
		AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
		return NULL;
	}

	CBComPtr<CBProcess> pProcess;
	pProcess.CreateInstance();
	pProcess->SetHandle(hProcess);
	return pProcess.Detach();
}

LPDISPATCH CNetBox2App::Exec(LPCTSTR pstrName, VARIANT* varCmdShow, VARIANT* varDirectory)
{
	long nCmdShow = SW_SHOWNORMAL;
	BOOL bAdmin = FALSE, bRedirect = FALSE;
	DWORD exitCode = 0;

	if(varCmdShow->vt != VT_ERROR)
		nCmdShow = varGetNumber(varCmdShow);

	bRedirect = nCmdShow & 0x40;
	if (!bRedirect)
	{
		OSVERSIONINFO  versionInfo;
		::ZeroMemory(&versionInfo, sizeof(OSVERSIONINFO));
		versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
		::GetVersionEx(&versionInfo);
		bAdmin = (nCmdShow & 0x20) > 0 && versionInfo.dwMajorVersion>=6;
	}

	nCmdShow &= 0xf;

	if (bRedirect)
	{
		STARTUPINFO StartupInfo;
		PROCESS_INFORMATION  ProcessInformation;

		ZeroMemory(&StartupInfo, sizeof(StartupInfo));
		ZeroMemory(&ProcessInformation, sizeof(ProcessInformation));
		StartupInfo.cb = sizeof(STARTUPINFO);
		StartupInfo.wShowWindow = (WORD)nCmdShow;
		StartupInfo.dwFlags = STARTF_USESHOWWINDOW;

		SECURITY_ATTRIBUTES saAttr;
		saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
		saAttr.bInheritHandle = TRUE;
		saAttr.lpSecurityDescriptor = NULL;

		CHandle hChildStdinRd, hChildStdinWr, hChildStdoutRd, hChildStdoutWr, hChildStderrRd, hChildStderrWr;
		// Create a pipe for the child process's STDERR. 
		if (!CreatePipe(&hChildStderrRd.m_h, &hChildStderrWr.m_h, &saAttr, 0))
		{
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
			return NULL;
		}
		// Ensure that the read handle to the child process's pipe for STDERR is not inherited.
		if (!SetHandleInformation(hChildStderrRd, HANDLE_FLAG_INHERIT, 0))
		{
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
			return NULL;
		}

		// Create a pipe for the child process's STDOUT. 
		if (!CreatePipe(&hChildStdoutRd.m_h, &hChildStdoutWr.m_h, &saAttr, 0))
		{
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
			return NULL;
		}
		// Ensure that the read handle to the child process's pipe for STDOUT is not inherited.
		if (!SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0))
		{
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
			return NULL;
		}

		// Create a pipe for the child process's STDIN. 
		if (!CreatePipe(&hChildStdinRd.m_h, &hChildStdinWr.m_h, &saAttr, 0))
		{
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
			return NULL;
		}
		// Ensure that the write handle to the child process's pipe for STDIN is not inherited. 
		if (!SetHandleInformation( hChildStdinWr, HANDLE_FLAG_INHERIT, 0))
		{
			AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
			return NULL;
		}

		StartupInfo.hStdError = hChildStderrWr;
		StartupInfo.hStdOutput = hChildStdoutWr;
		StartupInfo.hStdInput = hChildStdinRd;
		StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

		LPCTSTR lpDirectory = NULL;
		CStringA strDirectory;
		if(varDirectory->vt == VT_BSTR)
		{
			strDirectory = *varDirectory;
			if (PathIsDirectory(strDirectory))
				lpDirectory = strDirectory;
		}

		if(CreateProcess(NULL, (LPTSTR)pstrName, NULL, NULL, TRUE, 0, NULL, lpDirectory, &StartupInfo, &ProcessInformation))
		{
			CloseHandle(ProcessInformation.hThread);

			CBComPtr<CBProcess> pProcess;
			pProcess.CreateInstance();
			pProcess->SetHandle(ProcessInformation.hProcess, hChildStdinWr.Detach(), hChildStdoutRd.Detach(), hChildStderrRd.Detach());
			return pProcess.Detach();
		}

		AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
		return NULL;
	}

	SHELLEXECUTEINFO sei;
	BOOL bIsLong;
	CStringA strFile;

	while(isspace(*pstrName))
		pstrName ++;

	if(bIsLong = (*pstrName == '\"'))
		pstrName ++;

	LPCTSTR pstrFirst = pstrName;

	if(bIsLong)
		while(*pstrName != 0 && *pstrName != '\"')
			pstrName ++;
	else
		while(*pstrName != 0 && !isspace(*pstrName))
			pstrName ++;

	if(pstrName != pstrFirst)
	{
		strFile.SetString(pstrFirst, pstrName - pstrFirst);
		if (*pstrName != 0) pstrName++;
	}

	while(isspace(*pstrName))
		pstrName ++;

	ZeroMemory(&sei, sizeof(sei));
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	sei.nShow = nCmdShow;
	sei.lpFile = strFile;
	
	CStringA strDirectory;
	if(varDirectory->vt == VT_BSTR)
	{
		strDirectory = *varDirectory;
		if (PathIsDirectory(strDirectory))
			sei.lpDirectory = strDirectory;
	}

	sei.lpParameters = pstrName;
	if (bAdmin)
		sei.lpVerb = "runas";

	if(!ShellExecuteEx(&sei))
	{
		AfxThrowOleException(HRESULT_FROM_WIN32(GetLastError()));
		return NULL;
	}

	CBComPtr<CBProcess> pProcess;
	pProcess.CreateInstance();
	pProcess->SetHandle(sei.hProcess);
	return pProcess.Detach();
}

long CNetBox2App::Execute(LPCTSTR pstrName, VARIANT* varCmdShow)
{
	long nCmdShow = SW_SHOWNORMAL;
	BOOL bWait = FALSE, bAdmin = FALSE;
	DWORD exitCode = 0;

	if(varCmdShow->vt != VT_ERROR)
		nCmdShow = varGetNumber(varCmdShow);

	bWait = (nCmdShow & 0x10) > 0;

	OSVERSIONINFO  versionInfo;
	::ZeroMemory(&versionInfo, sizeof(OSVERSIONINFO));
	versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	::GetVersionEx(&versionInfo);
	bAdmin = (nCmdShow & 0x20) > 0 && versionInfo.dwMajorVersion>=6;

	nCmdShow &= 0xf;

	if (!bAdmin)
	{
		STARTUPINFO StartupInfo;
		PROCESS_INFORMATION  ProcessInformation;

		ZeroMemory(&StartupInfo, sizeof(StartupInfo));
		ZeroMemory(&ProcessInformation, sizeof(ProcessInformation));
		StartupInfo.cb = sizeof(STARTUPINFO);
		StartupInfo.wShowWindow = (WORD)nCmdShow;
		StartupInfo.dwFlags = STARTF_USESHOWWINDOW;

		if(CreateProcess(NULL, (LPTSTR)pstrName, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInformation))
		{
			if(bWait)
			{
				WaitForSingleObject(ProcessInformation.hProcess, INFINITE);
				GetExitCodeProcess(ProcessInformation.hProcess, &exitCode);
			}

			CloseHandle(ProcessInformation.hProcess);
			CloseHandle(ProcessInformation.hThread);

			return exitCode;
		}
	}
	SHELLEXECUTEINFO sei;
	BOOL bIsLong;
	CStringA strFile;

	while(isspace(*pstrName))
		pstrName ++;

	if(bIsLong = (*pstrName == '\"'))
		pstrName ++;

	LPCTSTR pstrFirst = pstrName;

	if(bIsLong)
		while(*pstrName != 0 && *pstrName != '\"')
			pstrName ++;
	else
		while(*pstrName != 0 && !isspace(*pstrName))
			pstrName ++;

	if(pstrName != pstrFirst)
	{
		strFile.SetString(pstrFirst, pstrName - pstrFirst);
		if (*pstrName != 0) pstrName++;
	}

	while(isspace(*pstrName))
		pstrName ++;

	ZeroMemory(&sei, sizeof(sei));
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	sei.nShow = nCmdShow;
	sei.lpFile = strFile;
	sei.lpParameters = pstrName;
	if (bAdmin)
		sei.lpVerb = "runas";

	if(!ShellExecuteEx(&sei))return -1;

	if(bWait)
	{
		WaitForSingleObject(sei.hProcess, INFINITE);
		GetExitCodeProcess(sei.hProcess, &exitCode);
	}

	CloseHandle(sei.hProcess);

	return exitCode;
}

void CNetBox2App::Shutdown(BOOL bReboot)
{
	HANDLE hToken;
	TOKEN_PRIVILEGES tkp;
	UINT uFlags = EWX_POWEROFF;

	if(bReboot)uFlags = EWX_REBOOT;

	if(ExitWindowsEx(uFlags, 0))return;
	if(ExitWindowsEx(uFlags | EWX_FORCE, 0))return;

	if (!OpenProcessToken(GetCurrentProcess(), 
		TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))return;
 
	LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
 
	tkp.PrivilegeCount = 1;
	tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED; 
 
	AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, (PTOKEN_PRIVILEGES)NULL, 0);

	if(IS_WINNT)
		InitiateSystemShutdown(NULL, NULL, 0, TRUE, bReboot);
	else if (!ExitWindowsEx(uFlags, 0))
		ExitWindowsEx(uFlags | EWX_FORCE, 0);
}

BSTR CNetBox2App::DoFileDialog(VARIANT* initFile, VARIANT* initDir, VARIANT* filter, VARIANT* title, BOOL bOpen)
{
	CBoxBSTR strIF, strID, strT;
	CString strFilter;

	OPENFILENAME ofn;
	char szFile[1024] = "";

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = ::GetForegroundWindow();
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.nFilterIndex = 1;

	if(initFile->vt != VT_ERROR)
	{
		strIF.Attach(initFile);
		strncpy(szFile, strIF.m_string, sizeof(szFile));
	}

	if(initDir->vt != VT_ERROR)
	{
		strID.Attach(initDir);
		ofn.lpstrInitialDir = (LPCTSTR)strID.m_string;
	}

	if(title->vt != VT_ERROR)
	{
		strT.Attach(title);
		ofn.lpstrTitle = (LPCTSTR)strT.m_string;
	}

	if(filter->vt != VT_ERROR)
	{
		CComVariant varF;

		varF.ChangeType(VT_BSTR, filter);
		if(varF.vt != VT_BSTR)AfxThrowOleException(TYPE_E_TYPEMISMATCH);

		int n = ::SysStringLen(varF.bstrVal) + 1;

		strFilter.ReleaseBuffer(::WideCharToMultiByte(_AtlGetConversionACP(), 0, varF.bstrVal, n, strFilter.GetBuffer(n * 2), n * 2, NULL, NULL));

		ofn.lpstrFilter = strFilter;
	}

	if(bOpen)
	{
		if (szFile[0] == ',')
		{
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
			szFile[0] = 0;
		}
		else
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
		if(!GetOpenFileName(&ofn))
			szFile[0] = 0;
		else
		{
			for (int x =0; x < 1023; x++)
			{
				if ((szFile[x] == NULL) && (szFile[x + 1] == NULL))
					break;
				if (szFile[x] == NULL)
					szFile[x] = ',';
			}
		}

	}else
	{
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
		if(!GetSaveFileName(&ofn))szFile[0] = 0;
	}

	return CString(szFile).AllocSysString();
}

BSTR CNetBox2App::OpenFileDialog(VARIANT* initFile, VARIANT* initDir, VARIANT* filter, VARIANT* title)
{
	return DoFileDialog(initFile, initDir, filter, title);
}

BSTR CNetBox2App::SaveFileDialog(VARIANT* initFile, VARIANT* initDir, VARIANT* filter, VARIANT* title)
{
	return DoFileDialog(initFile, initDir, filter, title, FALSE);
}

class _____tempData
{
public:
	CBoxBSTR strID, strT;
	BOOL bTitle;
};

static INT CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lp, LPARAM pData)
{
	_____tempData *pinfo = (_____tempData *)pData;

	switch(uMsg)
	{
	case BFFM_INITIALIZED:
		if(pinfo->bTitle)::SetWindowText(hwnd, (LPCTSTR)pinfo->strT.m_string);
		SendMessage(hwnd, BFFM_SETSELECTION, TRUE, (LPARAM)(LPCTSTR)pinfo->strID.m_string);
		break;
	}
	return 0;
}

BSTR CNetBox2App::BrowseForFolder(VARIANT* initDir, VARIANT* title, VARIANT* varDescription, VARIANT* varNew)
{
	BROWSEINFO bi;
	LPITEMIDLIST pidl;
	CComPtr<IMalloc> pMalloc;
	_____tempData td;
	CComVariant var;
	CBoxBSTR strDescription;

	ZeroMemory(&bi, sizeof(bi));
	bi.lpfn = BrowseCallbackProc;
	bi.hwndOwner = ::GetForegroundWindow();
	bi.lParam = (LPARAM)&td;

	if(initDir->vt != VT_ERROR)
		td.strID.Attach(initDir);

	if(td.bTitle = (title->vt != VT_ERROR))
		td.strT.Attach(title);

	if(varDescription->vt != VT_ERROR)
	{
		strDescription.Attach(varDescription);
		bi.lpszTitle = strDescription.m_string;
	}

	if(varNew->vt != VT_ERROR)
	{
		var.ChangeType(VT_BOOL, varNew);
		if(var.vt != VT_BOOL)AfxThrowOleException(TYPE_E_TYPEMISMATCH);
		if(var.boolVal)bi.ulFlags = BIF_NEWDIALOGSTYLE;
	}

	if (SUCCEEDED(SHGetMalloc(&pMalloc)))
	{
		pidl = SHBrowseForFolder(&bi);

		if (pidl)
		{
			TCHAR szDir[_MAX_PATH];

			if(SHGetPathFromIDList(pidl, szDir))
				td.strID.m_string = szDir;
			pMalloc->Free(pidl);
		}else td.strID.m_string.Empty();
	}else AfxThrowMemoryException();

	return td.strID.m_string.AllocSysString();
}

void CNetBox2App::DoEvents(void)
{
	MSG msg;

	while(PeekMessage( &msg, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage( &msg );
		DispatchMessage( &msg );
	}
}

CString CNetBox2App::GetContentTypeFromFileName(LPCTSTR szFileName)
{
	CString strType;

	TCHAR szExt[_MAX_EXT];
	_tsplitpath(szFileName, NULL, NULL, NULL, szExt);
	_tcslwr(szExt);

	CSingleLock l(&m_csMime, TRUE);
	m_mapMimeType.Lookup(szExt, strType);
	l.Unlock();

	if(!strType.IsEmpty())
		return strType;

	HKEY hItem;
	if (RegOpenKeyEx(HKEY_CLASSES_ROOT, szExt, 0, KEY_READ, &hItem) == ERROR_SUCCESS)
	{ 
		TCHAR sPath[_MAX_PATH];
		DWORD dwSize = _MAX_PATH;
		DWORD dwType = REG_SZ;

		if (RegQueryValueEx(hItem, _T("Content Type"), NULL, &dwType, (LPBYTE) sPath, &dwSize) == ERROR_SUCCESS)
			strType = sPath;

		RegCloseKey(hItem);
	}

	if(strType.IsEmpty())
		strType = _T("application/octet-stream");

	l.Lock();
	m_mapMimeType[szExt] = strType;
	l.Unlock();

	return strType;
}

long CNetBox2App::LoadPrivateKey(LPCTSTR PrivateKey, LPCTSTR pstrCertificate)
{
	if(m_pSSL_CTX->use_certificate_file(BOX_CT2CA(pstrCertificate)) <= 0)
		return -1;
	if(m_pSSL_CTX->use_PrivateKey_file(BOX_CT2CA(PrivateKey)) <= 0)
		return -1;

	if(!m_pSSL_CTX->check_private_key())
		return -1;

	return 0;
}
void CNetBox2App::Command(void)
{
	CBoxCommand cmd;

	cmd.Start();
}

void CNetBox2App::Start(void)
{
	//::CoInitializeEx(NULL, COINIT_MULTITHREADED);
	::CoInitialize(NULL);

	theApp.SetThreadName(_T("ScriptMain"));

	{
		CBThread th;

		m_pSystem.CreateInstance();

		CBoxObject<CBoxScript> pMainScript;

		pMainScript.CreateInstance();
		if(m_bStep)pMainScript->StepDebug();
		int nError = 0;

		if(m_bIsShell && g_pFile->m_strStartup.IsEmpty())
			Command();
		else nError = pMainScript->Load(g_pFile->m_strStartup);

		if(nError == 404)
		{
			if(m_bIsShell)
			{
				Command();
				nError = 0;
			}
		}else if(nError == 0)
		{
			pMainScript->AddNameItem(_T("Shell"), (LPDISPATCH)theApp.GetInterface(&IID_IDispatch));
			pMainScript->AddNameItem(_T("NetBox"), theApp.m_pSystem);
			nError = pMainScript->Run();
		}

		if(nError != 0)
		{
			CString str;

			if(nError != 404)
			{
				CBoxScript::CScriptError &error = pMainScript->GetErrorMessage();

				str = error.m_strSource;
				str += _T('\n');

				if(error.m_sCode != 0)
					str.AppendFormat(_T("Error Number: %08X\nFile: "), error.m_sCode);
				else str.Append(_T("File: "), 6);
				str += error.m_strFile;
				str.AppendFormat(_T("\nLine: %d\n"), error.m_nLine);
				str += error.m_strDescription;
			}else
			{
				str = _T("File \"");
				str += g_pFile->m_strStartup;
				str += _T("\" not found.");
			}

			if(m_bIsShell)
				MessageBox(NULL, str, CBoxSystem::getVersion(), MB_ICONSTOP | MB_OK);
			else m_pService->LogEvent(EVENTLOG_ERROR_TYPE, str);
		}

		pMainScript.Release();

		m_pSystem->m_pContents->RemoveAll();
		m_pSystem->ClearLock();
	}

	ERR_remove_state(0);

	::CoUninitialize();
}

UINT CNetBox2App::staticStart(void* p)
{
	theApp.Start();
	theApp.PostThreadMessage(WM_QUIT, 0, 0);

	return 0;
}

UINT CNetBox2App::staticStartService(void* p)
{
	theApp.SetThreadName(_T("ServiceMain"));
	CBThread th;

	theApp.m_pService->Dispatch(CBStringA(theApp.m_pArguments->GetString(2)));

	theApp.PostThreadMessage(WM_QUIT, 0, 0);
	return 0;
}

static LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter;
static __declspec(thread) TCHAR th_strThreadName[32];

void CNetBox2App::SetThreadName(LPCTSTR strName)
{
	_tcscpy(th_strThreadName, strName);
}

CBCriticalSection s_csFilter;

static LONG WINAPI MyUnhandledExceptionFilter(PEXCEPTION_POINTERS ep)
{
	s_csFilter.Enter();

	MEMORY_BASIC_INFORMATION bsi;
	char FileName[_MAX_PATH];
	char *pstr = "";
	DWORD dwSize;
	CString str;
	void* pstack;
	void** pBP;

	pstack = ep->ExceptionRecord->ExceptionAddress;
	pBP = (void**)ep->ContextRecord->Ebp;

	str.Format("%s\nUnexpected Error: %08X, %s", CBoxSystem::getVersion(), ep->ExceptionRecord->ExceptionCode, th_strThreadName);

//	do
//	{
		::VirtualQuery(pstack, &bsi, sizeof(bsi));
		if(dwSize = GetModuleFileName((HMODULE)bsi.AllocationBase, FileName, _MAX_PATH))
		{
			pstr = FileName + dwSize - 1;
			while(pstr > FileName && *pstr != '\\')
				pstr --;

			pstr ++;
		}

		str.AppendFormat("\n%s - %08X", pstr, pstack);

//		if(IsBadReadPtr(pBP, sizeof(DWORD) * 2))break;
//		if((void**)*pBP < pBP)break;

//		pstack = pBP[1];
//		pBP = (void**)*pBP;
//	}while(pstack);

	if(theApp.m_bIsShell)
		MessageBox(NULL, str, CBoxSystem::getVersion(), MB_ICONSTOP | MB_OK);
	theApp.m_pService->LogEvent(EVENTLOG_ERROR_TYPE, str);

	ExitProcess(0);

	return 0;
}

void CNetBox2App::LogEvent(long nType, LPCTSTR pstrMsg)
{
#ifndef LOGEVENT
	return;
#endif
	CString strFileName;
	TCHAR buffer[_MAX_PATH] = _T("");

	::GetModuleFileName(NULL, buffer, _MAX_PATH);
	strFileName = buffer;

	HANDLE hFile = ::CreateFile(strFileName+".evt", GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	if (hFile != INVALID_HANDLE_VALUE)
	{
		SetFilePointer(hFile, 0, NULL, FILE_END);

		SYSTEMTIME st;
		::GetLocalTime(&st);

		CString strMessage;
		strMessage.Format("%04d-%02d-%02d %02d:%02d:%02d\t%d\t%s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, nType, pstrMsg);

		DWORD dwWritten;
		WriteFile(hFile, strMessage, strMessage.GetLength(), &dwWritten, NULL);
		::CloseHandle(hFile);
	}
}

LONG WINAPI MyUnhandledFilter(PEXCEPTION_POINTERS lpExceptionInfo)
{
	CString strMessage, strFileName;

	SYSTEMTIME st;
	::GetLocalTime(&st);
	strFileName.Format(".%04d%02d%02d %02d%02d%02d.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	MoveFile(g_pFile->m_strAppName+".dmp", g_pFile->m_strAppName+strFileName);

	HANDLE hFile = ::CreateFile(g_pFile->m_strAppName+".dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	if (hFile != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION ExInfo;

		ExInfo.ThreadId = ::GetCurrentThreadId();
		ExInfo.ExceptionPointers = lpExceptionInfo;
		ExInfo.ClientPointers = false;

		BOOL bOK = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &ExInfo, NULL, NULL );

		if (bOK)
			strMessage = "Create Dump File Success!";
		else
			strMessage.Format("MiniDumpWriteDump Failed: %d!", GetLastError());

		::CloseHandle(hFile);
	}
	else
	{
		strMessage.Format("Create Dump File Failed %d!", GetLastError());
	}

	//if(theApp.m_bIsShell)
	//	MessageBox(NULL, strMessage, CBoxSystem::getVersion(), MB_ICONSTOP | MB_OK);
	theApp.m_pService->LogEvent(EVENTLOG_ERROR_TYPE, strMessage);

	return 0;
}

void CNetBox2App::CallProc(void (*proc)(void*), void* pParam, BOOL AsynCall)
{
	void* param[2] = {proc, pParam};

	if(!AsynCall)
	{
		m_csCallProc.Lock();
		PostThreadMessage(WM_USER, 0, (LPARAM)param);
		WaitForSingleObject(m_hCallProc, INFINITE);
		m_csCallProc.Unlock();
	}else PostThreadMessage(WM_USER + 1, 0, (LPARAM)param);
}

BOOL CNetBox2App::InitInstance()
{
	LogEvent(0, "CNetBox2App::InitInstance()");

	//s_previousFilter = SetUnhandledExceptionFilter(MyUnhandledExceptionFilter);
	s_previousFilter = SetUnhandledExceptionFilter(MyUnhandledFilter);

	SetThreadName(_T("Main"));

	WSADATA wsaData;
	WSAStartup(MAKEWORD(1,1), &wsaData);

	CWinApp::InitInstance();
	LogEvent(0, "CWinApp::InitInstance()");

	SSL_library_init();
	LogEvent(0, "SSL_library_init()");

	CRYPTO_thread_setup();
	LogEvent(0, "CRYPTO_thread_setup()");

	RAND_poll();
	LogEvent(0, "RAND_poll()");

	OpenSSL_add_all_ciphers();
	LogEvent(0, "OpenSSL_add_all_ciphers()");

	g_pFileSystem.CreateInstance();
	LogEvent(0, "g_pFileSystem.CreateInstance()");

	g_pFile = new CBoxZipFile();
	LogEvent(0, "g_pFile = new CBoxZipFile()");

	g_pFile = new CBoxCachePool(g_pFile);
	LogEvent(0, "g_pFile = new CBoxCachePool(g_pFile)");

	//::CoInitializeEx(NULL, COINIT_MULTITHREADED);
	AfxOleInit();
	LogEvent(0, "AfxOleInit()");

	DWORD dwConnNum = 0x10;
	//InternetSetOption(NULL, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &dwConnNum, sizeof(dwConnNum));
	LogEvent(0, "-InternetSetOption(NULL, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &dwConnNum, sizeof(dwConnNum))");

	dwConnNum = 0x20;
	//InternetSetOption(NULL, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER, &dwConnNum, sizeof(dwConnNum));
	LogEvent(0, "-InternetSetOption(NULL, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER, &dwConnNum, sizeof(dwConnNum))");

	CBHook::DoHook();
	LogEvent(0, "CBHook::DoHook()");

	_Module.Init(NULL, m_hInstance);
	LogEvent(0, "_Module.Init(NULL, m_hInstance)");

	CComPtr<IDataInitialize> pIDataInitialize;
	LogEvent(0, "CComPtr<IDataInitialize> pIDataInitialize");

	pIDataInitialize.CoCreateInstance(CLSID_MSDAINITIALIZE);
	LogEvent(0, "pIDataInitialize.CoCreateInstance(CLSID_MSDAINITIALIZE)");

	CBComPtr<IInternetSession> pSession;
	LogEvent(0, "CBComPtr<IInternetSession> pSession");

	CBComPtr< CBFactory<CBoxProtocol> > pFactory;
	LogEvent(0, "CBComPtr< CBFactory<CBoxProtocol> > pFactory");

	pFactory.Attach(new CBFactory<CBoxProtocol>);
	LogEvent(0, "pFactory.Attach(new CBFactory<CBoxProtocol>)");

	CoInternetGetSession(0, &pSession, 0);
	LogEvent(0, "CoInternetGetSession(0, &pSession, 0)");

	if (pSession && pFactory)
	{
		pSession->RegisterNameSpace(pFactory, IID_NULL, L"NETBOX", 0, 0, 0);
		LogEvent(0, "pSession->RegisterNameSpace(pFactory, IID_NULL, L\"NETBOX\", 0, 0, 0)");
	}

	m_pSSL_CTX = new CSSLContext;
	LogEvent(0, "m_pSSL_CTX = new CSSLContext");

	CBoxScriptObject::m_pGlobalObject.CoCreateInstance(CLSID_StdGlobalInterfaceTable);
	LogEvent(0, "CBoxScriptObject::m_pGlobalObject.CoCreateInstance");
	
	LogEvent(0, GetCommandLine());

	{
		CBThread th;
		MSG msg;

		m_pArguments.CreateInstance();
		m_pArguments->put_CommandLine((BSTR)(LPCWSTR)CBString(GetCommandLine()));

		LogEvent(0, "CommandLine parsing...");

		if(g_pFile->m_strStartup.IsEmpty())
		{
			if(m_pArguments->GetCount() >= 3 && !m_pArguments->GetString(1).CompareNoCase(L"-debug"))
			{
				LogEvent(0, "CommandLine Debug");
				
				g_pFile->SetRuntimeFile(CString(m_pArguments->GetString(2)));
				g_pFileSystem->SetRuntimeFile(m_pArguments->GetString(2));
				g_pFile->m_strStartup = (LPCTSTR)g_pFile->m_strAppName + (g_pFile->m_strBasePath.GetLength() - 1);
				m_pArguments->Remove(0);
				m_pArguments->Remove(0);
				m_bStep = TRUE;
			}else if(m_pArguments->GetCount() >= 3 && !m_pArguments->GetString(1).CompareNoCase(L"-run"))
			{
				LogEvent(0, "CommandLine Run");
				
				g_pFile->SetRuntimeFile(CString(m_pArguments->GetString(2)));
				g_pFileSystem->SetRuntimeFile(m_pArguments->GetString(2));
				g_pFile->m_strStartup = (LPCTSTR)g_pFile->m_strAppName + (g_pFile->m_strBasePath.GetLength() - 1);
				m_pArguments->Remove(0);
				m_pArguments->Remove(0);
			}else if(m_pArguments->GetCount() >= 2)
			{
				LogEvent(0, "CommandLine Other");

				g_pFile->SetRuntimeFile(CString(m_pArguments->GetString(1)));
				g_pFileSystem->SetRuntimeFile(m_pArguments->GetString(1));
				g_pFile->m_strStartup = (LPCTSTR)g_pFile->m_strAppName + (g_pFile->m_strBasePath.GetLength() - 1);
				m_pArguments->Remove(0);
			}
		}

		if(IS_WINNT && m_pArguments->GetCount() == 3 &&
			!m_pArguments->GetString(1).CompareNoCase(L"-Dispatch"))
		{
			LogEvent(0, "CommandLine NT Service");

			AfxBeginThread(staticStartService, 0);
		}
		else
		{
			LogEvent(0, "CommandLine Normal Start");

			AfxBeginThread(staticStart, 0);
			//staticStart(NULL);
		}

		while(GetMessage( &msg, NULL, 0, 0))
		{
			if(msg.message >= WM_USER && msg.hwnd == NULL && msg.wParam == 0)
			{
				if(msg.message == WM_USER)
				{
					((void (*)(void*))((void**)msg.lParam)[0])(((void**)msg.lParam)[1]);
					SetEvent(m_hCallProc);
				}else if(msg.message == WM_USER + 1)
					((void (*)(void*))((void**)msg.lParam)[0])(((void**)msg.lParam)[1]);
			}else
			{
				TranslateMessage( &msg );
				DispatchMessage( &msg );
			}
		}

		LogEvent(0, "Exit...");
		{
			char _buf[16];
			LogEvent(0, _itoa(m_nErrorCode, _buf, 10));
		}

		if(m_nErrorCode)
		{
			if(m_bRunSelfAtExit)
				if(IS_WINNT && m_pArguments->GetCount() == 3 &&
					!m_pArguments->GetString(1).CompareNoCase(L"-Dispatch"))
				{
					m_pService->m_strName = m_pArguments->GetString(2);
					m_pService->Start();
				}else WinExec(GetCommandLine(), SW_SHOW);

			ExitProcess(m_nErrorCode & 0x7fffffff);
		}
	}

	LogEvent(0, "Releasing...");

	if(m_pSystem != NULL)
	{
		m_pSystem->m_pContents->RemoveAll();
		m_pSystem->ClearLock();

		m_pSystem.Release();
	}

	g_pFileSystem.Release();

	CBoxScriptObject::m_pGlobalObject.Release();

	delete m_pSSL_CTX;

	_Module.Term();

	//g_cacheScript.RemoveAll();

	if(pSession && pFactory)
	{
		pSession->UnregisterNameSpace(pFactory, L"NETBOX");

		pFactory.Release();
		pSession.Release();
	}

	CBBrowserCaps::Clear();

	CBHook::DoHook(FALSE);

	if(g_pFile)delete g_pFile;

	CONF_modules_unload(1);
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
	ERR_remove_state(0);

	WSACleanup();

	SetUnhandledExceptionFilter(s_previousFilter);

	LogEvent(0, "CNetBox2App::InitInstance() End.");
	return FALSE;
}

extern int AFXAPI AfxWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
	__in LPTSTR lpCmdLine, int nCmdShow);

extern "C" int _tmain(int argc, TCHAR* argv[], TCHAR* envp[])
{
	return AfxWinMain(GetModuleHandle(NULL), NULL, GetCommandLine(), SW_SHOW);
}
