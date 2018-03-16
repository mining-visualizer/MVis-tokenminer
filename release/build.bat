@ECHO OFF

REM the expression %~dp0 returns the drive and folder in which this batch file is located
 
cd %~dp0


"C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe" /m:3 "..\build\MVis-tokenminer.sln" /t:Build /p:Configuration=Release
IF ERRORLEVEL 1 GOTO ERROR


REM Copy tokenminer.ini and Readme file 
copy "..\tokenminer.ini" stage
copy "..\Readme.md" stage

REM Copy binaries
copy "..\build\ethminer\release\tokenminer.exe" stage\tokenminer
copy "..\build\ethminer\release\libcurl.dll" stage\tokenminer
copy "..\build\ethminer\release\libmicrohttpd-dll.dll" stage\tokenminer
copy "..\build\ethminer\release\OpenCL.dll" stage\tokenminer

del *.zip
powershell.exe -nologo -noprofile -command "& { Add-Type -A 'System.IO.Compression.FileSystem'; [IO.Compression.ZipFile]::CreateFromDirectory('stage', 'mvis-tokenminer-ver-win64.zip'); }"

echo.
echo.
echo =============================================
echo.
echo.
echo All Done!
echo.
echo.
echo.
echo.
echo.
goto OUT

:ERROR

echo.
echo.
echo =============================================
echo.
echo.
echo ERRORS WERE ENCOUNTERED !!!
echo.
echo.
echo.
echo.
echo.

:OUT
pause