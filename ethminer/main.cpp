/*
	This file is part of mvis-ethereum.

	mvis-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	mvis-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mvis-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file main.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 * Ethereum client.
 */

#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim_all.hpp>
#include <libdevcore/FileSystem.h>

const std::string DonationAddress = "0xA804e933301AA2C919D3a9834082Cddda877C205";

#include "MinerAux.h"

// Solves the problem of including windows.h before including winsock.h
// as detailed here:
// http://stackoverflow.com/questions/1372480/c-redefinition-header-files-winsock2-h
#if defined(_WIN32)
#define _WINSOCKAPI_
#include <windows.h>
#endif

using namespace std;
using namespace dev;
using namespace dev::eth;
using namespace boost::algorithm;

#undef RETURN

MinerCLI ethminer;

void shutDown() 
{
	LogS << "Shutting down ...";
	ethminer.shutdown();
	this_thread::sleep_for(chrono::milliseconds(300));
}

#if defined(_WIN32)

BOOL CtrlHandlerWin(DWORD fdwCtrlType)
{
	// in Windows, this gets called on a separate thread. the program is terminated immediately
	// after this routine exits.
	shutDown();
	return(FALSE);
}

#else

void CtrlHandlerPosix(int s){
	shutDown();
	exit(1); 
}

#endif

void donations()
{
	cout
	<< " Please consider a donation to mining-visualizer.eth  (" << DonationAddress << ")" << endl
	<< " " << endl;
}

void help()
{
	cout
		<< endl
		<< " Usage: tokenminer [OPTIONS]" << endl << endl
		<< " Options:" << endl << endl;
	MinerCLI::streamHelp(cout);
	cout
		<< endl
		<< " General Options:" << endl
		<< "    -V,--version  Show the version and exit." << endl
		<< "    -h,--help  Show this help message and exit." << endl
		<< " " << endl
	;
	donations();
	exit(0);
}

void version()
{
	cout
	<< " " << endl
	<< " MVis-tokenminer " << dev::Version << endl
	<< " =====================================================================" << endl
	<< " Forked from github.com/Genoil/cpp-ethereum" << endl
	<< " " << endl;
	donations();
}

void SetCtrlCHandler()
{
#if defined(_WIN32)
	SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandlerWin, TRUE);
#else
	struct sigaction sigIntHandler;
	sigIntHandler.sa_handler = CtrlHandlerPosix;
	sigemptyset(&sigIntHandler.sa_mask);
	sigIntHandler.sa_flags = 0;
	sigaction(SIGINT, &sigIntHandler, NULL);
	sigaction(SIGTERM, &sigIntHandler, NULL);
	sigaction(SIGHUP, &sigIntHandler, NULL);
#endif
}

int main(int argc, char** argv)
{
	
	string arg = argc >= 2 ? argv[1] : "";
	if (arg == "-h" || arg == "--help")
	{
		help();
		exit(0);
	}
	else if (arg == "-V" || arg == "--version")
	{
		version();
		exit(0);
	}

	int i = 1;
	bool optionsLoaded;

	if (arg == "--config" && argc >= 3)
	{
		optionsLoaded = ProgOpt::Load(argv[2]);
		i = 3;
	}
	else
		optionsLoaded = ProgOpt::Load("");

	if (!optionsLoaded) 
		exit(-1);
	ethminer.loadIniSettings();

	for ( ; i < argc; ++i)
	{
		arg = argv[i];
		if (!ethminer.interpretOption(i, argc, argv))
		{
			cerr << "Invalid argument: " << arg << endl;
			exit(-1);
		}
	}

	SetCtrlCHandler();

	MultiLog::Init();

	version();
	ethminer.execute();

	return 0;
}

