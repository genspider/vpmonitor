/*++

Copyright (c) 1990-2003  Microsoft Corporation
All rights reserved

Module Name:

    localmon.c

--*/

#include "precomp.h"
#pragma hdrstop

#include <DriverSpecs.h>
__user_driver

#include <lmon.h>
#include <io.h>
#include "irda.h"

DWORD               dwCount = 0;                 
HANDLE              LcmhMonitor;
HINSTANCE           LcmhInst;
CRITICAL_SECTION    LcmSpoolerSection;

PMONITORINIT        g_pMonitorInit;

HANDLE hWrite;
FILE*  LogFile;
int    LogFlag = -1;

DWORD LcmPortInfo1Strings[]={FIELD_OFFSET(PORT_INFO_1, pName),
                          (DWORD)-1};

DWORD LcmPortInfo2Strings[]={FIELD_OFFSET(PORT_INFO_2, pPortName),
                          FIELD_OFFSET(PORT_INFO_2, pMonitorName),
                          FIELD_OFFSET(PORT_INFO_2, pDescription),
                          (DWORD)-1};
//自定义区
HANDLE hLogFile;

WCHAR g_szPortsKey[]  = L"Ports";
WCHAR gszWindows[]= L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows";
WCHAR szPortsEx[] = L"portsex"; /* Extra ports values */
WCHAR szNUL[]     = L"NUL";
WCHAR szNUL_COLON[] = L"NUL:";
WCHAR szFILE[]    = L"FILE:";
WCHAR szLcmCOM[]  = L"COM";
WCHAR szLcmLPT[]  = L"LPT";
WCHAR szIRDA[]    = L"IR";
WCHAR szLp[]      = L"lp";
extern DWORD g_COMWriteTimeoutConstant_ms;

BOOL
LocalMonInit(HINSTANCE hModule)
{

    LcmhInst = hModule;

    return InitializeCriticalSectionAndSpinCount(&LcmSpoolerSection, 0x80000000);
}


VOID
LocalMonCleanUp(
    VOID
    )
{

    DeleteCriticalSection(&LcmSpoolerSection);
}

BOOL
LcmEnumPorts(
    __in                        HANDLE  hMonitor,
    __in_opt                    LPWSTR  pName,
                                DWORD   Level,
    __out_bcount_opt(cbBuf)     LPBYTE  pPorts,
                                DWORD   cbBuf,
    __out                       LPDWORD pcbNeeded,
    __out                       LPDWORD pcReturned
    )
{
    PINILOCALMON    pIniLocalMon    = (PINILOCALMON)hMonitor;
    PLCMINIPORT     pIniPort        = NULL;
    DWORD           cb              = 0;
    LPBYTE          pEnd            = NULL;
    DWORD           LastError       = 0;

    UNREFERENCED_PARAMETER(pName);
	LOG_TRACE("this is called\r\n");
    LcmEnterSplSem();

    cb=0;

    pIniPort = pIniLocalMon->pIniPort;

    CheckAndAddIrdaPort(pIniLocalMon);

    while (pIniPort) {

        if ( !(pIniPort->Status & PP_FILEPORT) ) {

            cb+=GetPortSize(pIniPort, Level);
        }
        pIniPort=pIniPort->pNext;
    }

    *pcbNeeded=cb;

    if (cb <= cbBuf) {

        pEnd=pPorts+cbBuf;
        *pcReturned=0;

        pIniPort = pIniLocalMon->pIniPort;
        while (pIniPort) {

            if (!(pIniPort->Status & PP_FILEPORT)) {

                pEnd = CopyIniPortToPort(pIniPort, Level, pPorts, pEnd);

                if( !pEnd ){
                    LastError = GetLastError();
                    break;
                }

                switch (Level) {
                case 1:
                    pPorts+=sizeof(PORT_INFO_1);
                    break;
                case 2:
                    pPorts+=sizeof(PORT_INFO_2);
                    break;
                default:
                    LastError = ERROR_INVALID_LEVEL;
                    goto Cleanup;
                }
                (*pcReturned)++;
            }
            pIniPort=pIniPort->pNext;
        }

    } else

        LastError = ERROR_INSUFFICIENT_BUFFER;

Cleanup:
   LcmLeaveSplSem();

    if (LastError) {

        SetLastError(LastError);
        return FALSE;

    } else

        return TRUE;
}

BOOL
LcmOpenPort(
    __in    HANDLE  hMonitor,
    __in    LPWSTR  pName,
    __out   PHANDLE pHandle
    )
{
    PINILOCALMON    pIniLocalMon    = (PINILOCALMON)hMonitor;
    PLCMINIPORT     pIniPort        = NULL;
    BOOL            bRet            = FALSE;


    LcmEnterSplSem();

    if (!pName)
    {
       // LOG_TRACE("pName is null\r\n");
        SetLastError (ERROR_INVALID_PARAMETER);
        goto Done;
    }

    if (!IS_VP_PORT(pName))
    {
        LOG_TRACE("the %s is not a lp port \r\n", pName);

    }

    pIniPort = LcmCreatePortEntry( pIniLocalMon, pName );
    if ( !pIniPort )
    {
      //  LOG_TRACE("init port error\r\n");
		bRet = FALSE;
        goto Done;
    }
    pIniPort->Status |= PP_FILEPORT;
   // LOG_TRACE("init port success\r\n");
    *pHandle = pIniPort;
    bRet = TRUE;
    goto Done;

Done:
    if ( !bRet && pIniPort && (pIniPort->Status & PP_FILEPORT) )
        DeletePortNode(pIniLocalMon, pIniPort);

    if ( bRet )
        *pHandle = pIniPort;

    LcmLeaveSplSem();
    return bRet;
}

BOOL
LcmStartDocPort(
    __in    HANDLE  hPort,
    __in    LPWSTR  pPrinterName,
            DWORD   JobId,
            DWORD   Level,
    __in    LPBYTE  pDocInfo)
{
    PLCMINIPORT pIniPort        = (PLCMINIPORT)hPort;
   //PDOC_INFO_1 pDocInfo1       = (PDOC_INFO_1)pDocInfo;
    DWORD       Error           = 0;
	TCHAR       SymbolicLink[1024];

    HANDLE      hFile = INVALID_HANDLE_VALUE;
	UNREFERENCED_PARAMETER(pDocInfo);
    UNREFERENCED_PARAMETER(Level);


    if (pIniPort->Status & PP_STARTDOC)
    {
        return TRUE;
    }

    LcmEnterSplSem();
    pIniPort->Status |= PP_STARTDOC;
    LcmLeaveSplSem();

    pIniPort->hPrinter = NULL;
    pIniPort->pPrinterName = AllocSplStr(pPrinterName);
	LOG_TRACE("Printer: %S \r\n", pIniPort->pPrinterName);

    if (pIniPort->pPrinterName)
    {
        if (OpenPrinter(pPrinterName, &pIniPort->hPrinter, NULL))
        {
			memset(SymbolicLink, 0, sizeof(SymbolicLink));
            StringCchPrintf(SymbolicLink, 1024, PIPE_RAW_TRANSFER, pIniPort->pName);
            pIniPort->JobId = JobId;

            if(!WaitNamedPipe(SymbolicLink, NMPWAIT_USE_DEFAULT_WAIT))
            {
                goto Fail;
            }

            hFile = CreateFile(SymbolicLink,                   
                 GENERIC_WRITE,
                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                 NULL,
                 OPEN_ALWAYS,
                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                 NULL );

            if (hFile != INVALID_HANDLE_VALUE)
            {
                SetEndOfFile(hFile);
            }

            pIniPort->hFile = hFile;
        }
        
    }
  
    if (pIniPort->hFile == INVALID_HANDLE_VALUE)
    {
        LOG_TRACE("create file error\r\n");
        goto Fail;
    }

	LOG_TRACE("create file success\r\n");
    return TRUE;

Fail:
    SPLASSERT(pIniPort->hFile == INVALID_HANDLE_VALUE);

    LcmEnterSplSem();
    pIniPort->Status &= ~PP_STARTDOC;
    LcmLeaveSplSem();

    if (pIniPort->hPrinter)
    {
        ClosePrinter(pIniPort->hPrinter);
    }

    if (pIniPort->pPrinterName)
    {
        FreeSplStr(pIniPort->pPrinterName);
    }

    if (Error)
    {
        SetLastError(Error);
    }
    return FALSE;
}


BOOL
LcmWritePort(
            __in                HANDLE  hPort,
            __in_bcount(cbBuf)  LPBYTE  pBuffer,
            DWORD   cbBuf,
            __out               LPDWORD pcbWritten)
{
    PLCMINIPORT pIniPort    = (PLCMINIPORT)hPort;
    BOOL        rc          = FALSE;

	LOG_TRACE("this is called\r\n");
    if ( IS_IRDA_PORT(pIniPort->pName) )
    {
        rc = IrdaWritePort(pIniPort, pBuffer, cbBuf, pcbWritten);
    }
    else if (IS_COM_PORT(pIniPort->pName))
    {
        if (GetCOMPort(pIniPort))
        {
            rc = WriteFile(pIniPort->hFile, pBuffer, cbBuf, pcbWritten, NULL);
            if ( rc && *pcbWritten == 0 )
            {

                SetLastError(ERROR_TIMEOUT);
                rc = FALSE;
            }

            ReleaseCOMPort(pIniPort);
        }
    }
    else if ( !pIniPort->hFile || pIniPort->hFile == INVALID_HANDLE_VALUE )
    {

        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    else
    {
        rc = WriteFile(pIniPort->hFile, pBuffer, cbBuf, pcbWritten, NULL);
        if ( rc && *pcbWritten == 0 )
        {

            SetLastError(ERROR_TIMEOUT);
            rc = FALSE;
        }
    }


    return rc;
}


BOOL
LcmReadPort(
    __in                HANDLE      hPort,
    __out_bcount(cbBuf) LPBYTE      pBuffer,
                        DWORD       cbBuf,
    __out               LPDWORD     pcbRead)
{
	PLCMINIPORT pIniPort = (PLCMINIPORT)hPort;
    BOOL        rc          = FALSE;

	LOG_TRACE("this is called\r\n");
    // since COM ports can be read outside of StartDoc/EndDoc
    // additional work needs to be done.

	if (IS_VP_PORT(pIniPort->pName))
	{
		SetLastError(ERROR_SUCCESS);
		return rc;
	}

    if (IS_COM_PORT(pIniPort->pName))
    {
        if (GetCOMPort(pIniPort))
        {
            rc = ReadFile(pIniPort->hFile, pBuffer, cbBuf, pcbRead, NULL);


            ReleaseCOMPort(pIniPort);
        }
    }
    else if ( pIniPort->hFile &&
         (pIniPort->hFile != INVALID_HANDLE_VALUE) &&
         pIniPort->Status & PP_COMM_PORT )
    {
        rc = ReadFile(pIniPort->hFile, pBuffer, cbBuf, pcbRead, NULL);

	}
    else
    {
        SetLastError(ERROR_INVALID_HANDLE);
    }
	LOG_TRACE("this rslt is %d\r\n", rc);
    return rc;
}

BOOL
LcmEndDocPort(
    __in    HANDLE   hPort
    )
{
    PLCMINIPORT    pIniPort = (PLCMINIPORT)hPort;

	LOG_TRACE("this is called\r\n");
    if (!(pIniPort->Status & PP_STARTDOC))
    {
        return TRUE;
    }

    // The flush here is done to make sure any cached IO's get written
    // before the handle is closed.   This is particularly a problem
    // for Intelligent buffered serial devices
    if (pIniPort->hFile != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(pIniPort->hFile);
    }
   
    //
    // For any ports other than real LPT ports we open during StartDocPort
    // and close it during EndDocPort
    //
    if(!IS_VP_PORT(pIniPort->pName))
    {
        LOG_TRACE("this port:%S is not lp\r\n", pIniPort->pName);
        return FALSE;
    }

    CloseHandle(pIniPort->hFile);

    pIniPort->hFile = INVALID_HANDLE_VALUE;

    SetJob(pIniPort->hPrinter, pIniPort->JobId, 0, NULL, JOB_CONTROL_SENT_TO_PRINTER);

    ClosePrinter(pIniPort->hPrinter);

    FreeSplStr(pIniPort->pPrinterName);

    //
    // Startdoc no longer active.
    //
    pIniPort->Status &= ~PP_STARTDOC;

    return TRUE;
}

BOOL
LcmClosePort(
    __in    HANDLE  hPort
    )
{
   
    PLCMINIPORT pIniPort = (PLCMINIPORT)hPort;


    FreeSplStr(pIniPort->pDeviceName);
    pIniPort->pDeviceName = NULL;
    LcmEnterSplSem();
    DeletePortNode(pIniPort->pIniLocalMon, pIniPort);
    LcmLeaveSplSem();
	LOG_TRACE("this is called\r\n");
    return TRUE;
}


BOOL
LcmAddPortEx(
    __in        HANDLE   hMonitor,
    __in_opt    LPWSTR   pName,
                DWORD    Level,
    __in        LPBYTE   pBuffer,
    __in_opt    LPWSTR   pMonitorName
)
{
    PINILOCALMON    pIniLocalMon    = (PINILOCALMON)hMonitor;
    LPWSTR          pPortName       = NULL;
    DWORD           Error           = NO_ERROR;
    LPPORT_INFO_1   pPortInfo1      = NULL;
    LPPORT_INFO_FF  pPortInfoFF     = NULL;

    UNREFERENCED_PARAMETER(pMonitorName);
	LOG_TRACE("this is called\r\n");

    switch (Level) {
    case (DWORD)-1:
        pPortInfoFF = (LPPORT_INFO_FF)pBuffer;
        pPortName = pPortInfoFF->pName;
        break;

    case 1:
        pPortInfo1 =  (LPPORT_INFO_1)pBuffer;
        pPortName = pPortInfo1->pName;
        break;

    default:
        SetLastError(ERROR_INVALID_LEVEL);
        return(FALSE);
    }
    if (!pPortName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return(FALSE);
    }
    if (PortExists(pName, pPortName, &Error)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return(FALSE);
    }
    if (Error != NO_ERROR) {
        SetLastError(Error);
        return(FALSE);
    }
    if (!LcmCreatePortEntry(pIniLocalMon, pPortName)) {
        return(FALSE);
    }

    if (!AddPortInRegistry (pPortName)) {
        LcmDeletePortEntry( pIniLocalMon, pPortName );
        return(FALSE);
    }
    return TRUE;
}

BOOL
LcmGetPrinterDataFromPort(
    __in                            HANDLE  hPort,
                                    DWORD   ControlID,
    __in_opt                        LPWSTR  pValueName,
    __in_bcount_opt(cbInBuffer)     LPWSTR  lpInBuffer,
                                    DWORD   cbInBuffer,
    __out_bcount_opt(cbOutBuffer)   LPWSTR  lpOutBuffer,
                                    DWORD   cbOutBuffer,
    __out                           LPDWORD lpcbReturned)
{
    PLCMINIPORT  pIniPort  = (PLCMINIPORT)hPort;
    BOOL         rc        = FALSE;

    UNREFERENCED_PARAMETER(pValueName);

	LOG_TRACE("this is called\r\n");
    if (ControlID &&
        (pIniPort->Status & PP_DOSDEVPORT) &&
        IS_COM_PORT(pIniPort->pName))
    {
        if (GetCOMPort(pIniPort))
        {
            rc = DeviceIoControl(pIniPort->hFile,
                                ControlID,
                                lpInBuffer,
                                cbInBuffer,
                                lpOutBuffer,
                                cbOutBuffer,
                                lpcbReturned,
                                NULL);

            ReleaseCOMPort(pIniPort);
        }
    }
    else if (ControlID &&
             pIniPort->hFile &&
             (pIniPort->hFile != INVALID_HANDLE_VALUE) &&
             (pIniPort->Status & PP_DOSDEVPORT))
    {
        rc = DeviceIoControl(pIniPort->hFile,
                            ControlID,
                            lpInBuffer,
                            cbInBuffer,
                            lpOutBuffer,
                            cbOutBuffer,
                            lpcbReturned,
                            NULL);
    }
    else
    {
        SetLastError(ERROR_INVALID_PARAMETER);
    }

    return rc;
}

BOOL
LcmSetPortTimeOuts(
    __in        HANDLE          hPort,
    __in        LPCOMMTIMEOUTS  lpCTO,
    __reserved  DWORD           reserved)    // must be set to 0
{
    PLCMINIPORT     pIniPort    = (PLCMINIPORT)hPort;
    COMMTIMEOUTS    cto         = {0};
    BOOL            rc          = FALSE;

	LOG_TRACE("this is called\r\n");
    if (reserved != 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto done;
    }

    if ( !(pIniPort->Status & PP_DOSDEVPORT) )
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto done;
    }

    if (IS_COM_PORT(pIniPort->pName))
    {
        GetCOMPort(pIniPort);
    }

    if ( GetCommTimeouts(pIniPort->hFile, &cto) )
    {
        cto.ReadTotalTimeoutConstant = lpCTO->ReadTotalTimeoutConstant;
        cto.ReadIntervalTimeout = lpCTO->ReadIntervalTimeout;
        rc = SetCommTimeouts(pIniPort->hFile, &cto);
    }

    if (IS_COM_PORT(pIniPort->pName))
    {
        ReleaseCOMPort(pIniPort);
    }

done:
    return rc;
}

VOID
LcmShutdown(
    __in    HANDLE hMonitor
    )
{
    PLCMINIPORT     pIniPort        = NULL;
    PLCMINIPORT     pIniPortNext    = NULL;
    PINILOCALMON    pIniLocalMon    = (PINILOCALMON)hMonitor;

    LOG_TRACE("this is called\r\n");
    //
    // Delete the ports, then delete the LOCALMONITOR.
    //
    for( pIniPort = pIniLocalMon->pIniPort; pIniPort; pIniPort = pIniPortNext ){
        pIniPortNext = pIniPort->pNext;
        FreeSplMem( pIniPort );
    }

    FreeSplMem( pIniLocalMon );
}

DWORD
GetPortStrings(
    __out   PWSTR   *ppPorts
    )
{
    DWORD   sRetval  = ERROR_INVALID_PARAMETER;

	LOG_TRACE("this is called\r\n");
    if (ppPorts)
    {
        DWORD dwcValues = 0;
        DWORD dwMaxValueName = 0;
        HKEY  hk = NULL;

        //
        // open ports key
        //
        sRetval = g_pMonitorInit->pMonitorReg->fpCreateKey(g_pMonitorInit->hckRegistryRoot,
                                                           g_szPortsKey,
                                                           REG_OPTION_NON_VOLATILE,
                                                           KEY_READ,
                                                           NULL,
                                                           &hk,
                                                           NULL,
                                                           g_pMonitorInit->hSpooler);
        if (sRetval == ERROR_SUCCESS && hk)
        {
            sRetval = g_pMonitorInit->pMonitorReg->fpQueryInfoKey(hk,
                                                                  NULL,
                                                                  NULL,
                                                                  &dwcValues,
                                                                  &dwMaxValueName,
                                                                  NULL,
                                                                  NULL,
                                                                  NULL,
                                                                  g_pMonitorInit->hSpooler);
            if ((sRetval == ERROR_SUCCESS) && (dwcValues > 0))
            {
                PWSTR pPorts = NULL;
                DWORD cbMaxMemNeeded = ((dwcValues * (dwMaxValueName + 1) + 1) * sizeof(WCHAR));

                pPorts = (LPWSTR)AllocSplMem(cbMaxMemNeeded);

                if (pPorts)
                {
                    DWORD sTempRetval = ERROR_SUCCESS;
                    DWORD CharsAvail = cbMaxMemNeeded/sizeof(WCHAR);
                    INT   cIndex = 0;
                    PWSTR pPort = NULL;
                    DWORD dwCurLen = 0;

                    for (pPort = pPorts; sTempRetval == ERROR_SUCCESS; cIndex++)
                    {
                        dwCurLen = CharsAvail;
                        sTempRetval = g_pMonitorInit->pMonitorReg->fpEnumValue(hk,
                                                                               cIndex,
                                                                               pPort,
                                                                               &dwCurLen,
                                                                               NULL,
                                                                               NULL,
                                                                               NULL,
                                                                               g_pMonitorInit->hSpooler);
                        // based on the results of current length,
                        // move pointers/counters for the next iteration.
                        if (sTempRetval == ERROR_SUCCESS)
                        {
                            // RegEnumValue only returns the char count.
                            // Add 1 for NULL.
                            dwCurLen++;

                            // decrement the count of available chars.
                            CharsAvail -= dwCurLen;

                            // prepare pPort for next string.
                            pPort += dwCurLen;
                        }

                    }

                    if (sTempRetval == ERROR_NO_MORE_ITEMS)
                    {
                        *pPort = L'\0';
                        *ppPorts = pPorts;
                    }
                    else
                    {
                        // set return value in error case.
                        sRetval = sTempRetval;
                    }
                }
                else
                {
                    sRetval = GetLastError();
                }
            }

            // close Reg key.
            g_pMonitorInit->pMonitorReg->fpCloseKey(hk,
                                                    g_pMonitorInit->hSpooler);
        }
    }

    return sRetval;
}


MONITOR2 Monitor2 = {
    sizeof(MONITOR2),
    LcmEnumPorts,
    LcmOpenPort,
    NULL,           // OpenPortEx is not supported
    LcmStartDocPort,
    LcmWritePort,
    LcmReadPort,
    LcmEndDocPort,
    LcmClosePort,
    NULL,           // AddPort is not supported
    LcmAddPortEx,
    NULL,           // ConfigurePort is not supported
    NULL,           // DeletePort is not supported
    LcmGetPrinterDataFromPort,
    LcmSetPortTimeOuts,
    LcmXcvOpenPort,
    LcmXcvDataPort,
    LcmXcvClosePort,
    LcmShutdown
};


LPMONITOR2
InitializePrintMonitor2(
    __in    PMONITORINIT pMonitorInit,
    __out   PHANDLE phMonitor
    )
{
    LPWSTR   pPortTmp = NULL;
    DWORD    rc = 0, i = 0;
    PINILOCALMON pIniLocalMon = NULL;
    LPWSTR   pPorts = NULL;
    DWORD    sRetval = ERROR_SUCCESS;


    // cache pointer
    pIniLocalMon = (PINILOCALMON)AllocSplMem( sizeof( INILOCALMON ));
    if( !pIniLocalMon )
    {
        goto Fail;
    }

    pIniLocalMon->signature = ILM_SIGNATURE;
    pIniLocalMon->pMonitorInit = pMonitorInit;
    g_pMonitorInit = pMonitorInit;

    // get ports
    sRetval = GetPortStrings(&pPorts);
    if (sRetval != ERROR_SUCCESS)
    {
        SetLastError(sRetval);
        goto Fail;
    }


    LcmEnterSplSem();

    //
    // We now have all the ports
    //
    for(pPortTmp = pPorts; pPortTmp && *pPortTmp; pPortTmp += rc + 1){

        rc = (DWORD)wcslen(pPortTmp);

        if (!_wcsnicmp(pPortTmp, L"Ne", 2)) {

            i = 2;

            //
            // For Ne- ports
            //
            if ( rc > 2 && pPortTmp[2] == L'-' )
                ++i;
            for ( ; i < rc - 1 && iswdigit(pPortTmp[i]) ; ++i )
            ;

            if ( i == rc - 1 && pPortTmp[rc-1] == L':' ) {
                continue;
            }
        }

        LcmCreatePortEntry(pIniLocalMon, pPortTmp);
    }

    FreeSplMem(pPorts);

    LcmLeaveSplSem();

    CheckAndAddIrdaPort(pIniLocalMon);

    *phMonitor = (HANDLE)pIniLocalMon;


    return &Monitor2;

Fail:

    FreeSplMem( pPorts );
    FreeSplMem( pIniLocalMon );

    return NULL;
}

BOOL
DllMain(
    HINSTANCE hModule,
    DWORD dwReason,
    LPVOID lpRes)
{

	static BOOL bLocalMonInit = FALSE;
	InitLog();
    
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:

        bLocalMonInit = LocalMonInit(hModule);
        DisableThreadLibraryCalls(hModule);
        return TRUE;

    case DLL_PROCESS_DETACH:

        if (bLocalMonInit)
        {
            LocalMonCleanUp();
            bLocalMonInit = FALSE;
        }

        return TRUE;
    }

    UNREFERENCED_PARAMETER(lpRes);
	DoneLog();
    return TRUE;
}

BOOL 
InitLog()
{
	SECURITY_ATTRIBUTES sa;
	SECURITY_DESCRIPTOR sd;

	InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);

	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = FALSE;
	sa.lpSecurityDescriptor = &sd;

	hWrite = CreateFile(
		LOG_PATH,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		&sa,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL
		);

	if (INVALID_HANDLE_VALUE == hWrite)
	{
		return FALSE;
	}
	
	//0x8表示在文件末尾添加的意思, _O_APPEND ,fcntl.h;
	LogFlag = _open_osfhandle((intptr_t)hWrite, 0x0008);

	if (LogFlag<0)
	{
		return FALSE;
	}

	LogFile = _fdopen(LogFlag, "a+");
	if (!LogFile)
	{
		return FALSE;
	}

	*stdout = *LogFile;
	setvbuf(stdout, NULL, _IONBF, 0);
	
	return TRUE;
}

VOID
DoneLog()
{

	if (LogFile)
	{
		fflush(LogFile);
		fclose(LogFile);
	}

	if (-1 != LogFlag)
	{
		_close(LogFlag);
	}

	if (hWrite)
	{
		CloseHandle(hWrite);
	}
}