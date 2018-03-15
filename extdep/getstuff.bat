@echo off
REM get stuff!

REM cd to the folder this file is in
cd %~dp0

echo Downloading dependencies

if not exist download mkdir download 
if not exist install mkdir install
if not exist install\windows mkdir install\windows

set eth_server=https://github.com/ethereum/cpp-dependencies/releases/download/vc120

call :download boost 1.55.0 %eth_server%
call :download cryptopp 5.6.2 %eth_server%
call :download jsoncpp 1.6.2 %eth_server%
call :download json-rpc-cpp 0.5.0 %eth_server%
call :download leveldb 1.2 %eth_server%
call :download microhttpd 0.9.2 %eth_server%

set mvis_server=https://github.com/mining-visualizer/dependencies/releases/download/vc120

call :download curl 7.54.1 %mvis_server%
call :download OpenCL 1 %mvis_server%
call :download amd_adl 10.1 %mvis_server%

echo.
echo Operation is complete
pause

goto :EOF

:download

set eth_name=%1
set eth_version=%2
set server=%3

cd download

if not exist %eth_name%-%eth_version%-x64.tar.gz (
	echo.
	echo Downloading %server%/%eth_name%-%eth_version%-x64.tar.gz ...
	echo.
	
	..\curl -L -o %eth_name%-%eth_version%-x64.tar.gz %server%/%eth_name%-%eth_version%-x64.tar.gz
)
if not exist %eth_name%-%eth_version% cmake -E tar -zxvf %eth_name%-%eth_version%-x64.tar.gz
cmake -E copy_directory %eth_name%-%eth_version% ..\install\windows

cd ..

goto :EOF


