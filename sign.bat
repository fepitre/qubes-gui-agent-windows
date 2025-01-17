@ECHO OFF

IF NOT EXIST SIGN_CONFIG.BAT GOTO DONT_SIGN

IF %_BUILDARCH%==x86 (SET BUILDDIR=i386) ELSE (SET BUILDDIR=amd64)
IF %DDK_TARGET_OS%==Win2K SET SIGN_OS=2000
IF %DDK_TARGET_OS%==WinXP SET SIGN_OS=XP_X86
IF %DDK_TARGET_OS%%_BUILDARCH%==WinNETx86 SET SIGN_OS=Server2003_X86
IF %DDK_TARGET_OS%%_BUILDARCH%==WinNETAMD64 SET SIGN_OS=XP_X64,Server2003_X64
IF %DDK_TARGET_OS%%_BUILDARCH%==WinLHx86 SET SIGN_OS=Vista_X86,Server2008_X86
IF %DDK_TARGET_OS%%_BUILDARCH%==WinLHAMD64 SET SIGN_OS=Vista_X64,Server2008_X64
IF %DDK_TARGET_OS%%_BUILDARCH%==Win7x86 SET SIGN_OS=7_X86
IF %DDK_TARGET_OS%%_BUILDARCH%==Win7AMD64 SET SIGN_OS=7_X64,Server2008R2_X64

ECHO DDK_TARGET_OS=%DDK_TARGET_OS%
ECHO _BUILDARCH=%_BUILDARCH%
ECHO BUILDDIR=%BUILDDIR%
ECHO SIGN_OS=%SIGN_OS%

copy qvideo\inf\qvideo.inf.version qvideo\bin\%BUILDDIR%\qvideo.inf

%SIGNTOOL% sign /v %CERT_CROSS_CERT_FLAG% /f %CERT_FILENAME% %CERT_PASSWORD_FLAG% /t http://timestamp.verisign.com/scripts/timestamp.dll qvideo\bin\%BUILDDIR%\qvmini.sys
%SIGNTOOL% sign /v %CERT_CROSS_CERT_FLAG% /f %CERT_FILENAME% %CERT_PASSWORD_FLAG% /t http://timestamp.verisign.com/scripts/timestamp.dll qvideo\bin\%BUILDDIR%\qvgdi.dll
%DDK_PATH%\bin\selfsign\inf2cat /driver:qvideo\bin\%BUILDDIR% /os:%SIGN_OS%
%SIGNTOOL% sign /v %CERT_CROSS_CERT_FLAG% /f %CERT_FILENAME% %CERT_PASSWORD_FLAG% /t http://timestamp.verisign.com/scripts/timestamp.dll qvideo\bin\%BUILDDIR%\qvideo.cat

:DONT_SIGN
