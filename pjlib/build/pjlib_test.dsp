# Microsoft Developer Studio Project File - Name="pjlib_test" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103

CFG=pjlib_test - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "pjlib_test.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "pjlib_test.mak" CFG="pjlib_test - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "pjlib_test - Win32 Release" (based on "Win32 (x86) Console Application")
!MESSAGE "pjlib_test - Win32 Debug" (based on "Win32 (x86) Console Application")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""$/pjproject/pjlib/build", UIAAAAAA"
# PROP Scc_LocalPath "."
CPP=cl.exe
RSC=rc.exe

!IF  "$(CFG)" == "pjlib_test - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir ".\output\pjlib-test-i386-win32-vc6-release"
# PROP BASE Intermediate_Dir ".\output\pjlib-test-i386-win32-vc6-release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir ".\output\pjlib-test-i386-win32-vc6-release"
# PROP Intermediate_Dir ".\output\pjlib-test-i386-win32-vc6-release"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /c
# ADD CPP /nologo /MD /W4 /GX /Zi /O2 /I "../include" /D "NDEBUG" /D "WIN32" /D "_CONSOLE" /D "_MBCS" /D "PJ_WIN32" /D "PJ_M_I386" /FR /YX /FD /c
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
# ADD LINK32 netapi32.lib mswsock.lib ws2_32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /debug /machine:I386 /out:"../bin/pjlib-test-i386-win32-vc6-release.exe"

!ELSEIF  "$(CFG)" == "pjlib_test - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir ".\output\pjlib-test-i386-win32-vc6-debug"
# PROP BASE Intermediate_Dir ".\output\pjlib-test-i386-win32-vc6-debug"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir ".\output\pjlib-test-i386-win32-vc6-debug"
# PROP Intermediate_Dir ".\output\pjlib-test-i386-win32-vc6-debug"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /GZ /c
# ADD CPP /nologo /MTd /W4 /Gm /GX /ZI /Od /I "../include" /D "_DEBUG" /D "WIN32" /D "_CONSOLE" /D "_MBCS" /D "PJ_WIN32" /D "PJ_M_I386" /FR /YX /FD /GZ /c
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /debug /machine:I386 /pdbtype:sept
# ADD LINK32 netapi32.lib mswsock.lib ws2_32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /debug /machine:I386 /out:"../bin/pjlib-test-i386-win32-vc6-debug.exe" /pdbtype:sept

!ENDIF 

# Begin Target

# Name "pjlib_test - Win32 Release"
# Name "pjlib_test - Win32 Debug"
# Begin Group "Source Files"

# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"
# Begin Source File

SOURCE="..\src\pjlib-test\atomic.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\echo_clt.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\errno.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\exception.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\fifobuf.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\ioq_perf.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\ioq_tcp.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\ioq_udp.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\list.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\main.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\main_mod.c"
# PROP Exclude_From_Build 1
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\mutex.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\os.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\pool.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\pool_perf.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\rand.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\rbtree.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\select.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\sleep.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\sock.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\sock_perf.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\string.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\test.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\thread.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\timer.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\timestamp.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\udp_echo_srv_ioqueue.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\udp_echo_srv_sync.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\util.c"
# End Source File
# Begin Source File

SOURCE="..\src\pjlib-test\xml.c"
# End Source File
# End Group
# Begin Group "Header Files"

# PROP Default_Filter "h;hpp;hxx;hm;inl"
# Begin Source File

SOURCE="..\src\pjlib-test\test.h"
# End Source File
# End Group
# Begin Group "Resource Files"

# PROP Default_Filter "ico;cur;bmp;dlg;rc2;rct;bin;rgs;gif;jpg;jpeg;jpe"
# End Group
# End Target
# End Project
