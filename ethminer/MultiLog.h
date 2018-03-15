
#pragma once

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

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <boost/filesystem.hpp>
#include <libdevcore/Guards.h>
#include <ethminer/Common.h>

enum LogMode {LogOn, LogOff, LogFiltered};

class MultiLog
{

public:
	static void Init();
	MultiLog(LogMode _screenMode, LogMode _diskMode);
	MultiLog(int _xpos, int _ypos);
	~MultiLog();

public:
	template <class T> MultiLog& operator<<(T const& _t) { m_sstr << _t; return *this; }

private:
	static void loadFilters();
	static void trimLogFile();
	// this is static due to multi-threading
	static void simpleDebugOut(std::ostream& _os, std::string const& _s, bool _time, bool _eol);
	int filterMatch(std::string _s);
	void clearYExtent();
	static int getXY(int& x, int& y);
	static int getXPos();
	static int getYPos();
	void GotoXY(int x, int y);
	void clearLine(int _line);
	int getBufferHeight();

private:
	std::stringstream m_sstr;	///< The accrued log entry.
	LogMode m_screenMode;
	LogMode m_diskMode;
	bool m_positioned;
	int m_xpos, m_ypos;

	static boost::filesystem::path m_logFilename;
	static boost::filesystem::path m_filterFilename;
	static std::vector<std::string> m_filters;
	static SteadyClock::time_point m_filterCheckTime;
	static std::time_t m_filterChangeTime;
	static Mutex x_screenOutput;
	static Mutex x_diskOutput;
	static Mutex x_filter;
	static std::ofstream m_file;
	static int m_currentYBase;
	static int m_currentYExtent;
	static int m_rogueCatcher;

};




#define LogD MultiLog(LogOff, LogOn)			// log to Disk only, no filtering
#define LogF MultiLog(LogOff, LogFiltered)		// log to disk only, Filtered
#define LogB MultiLog(LogOn, LogOn)				// log to Both screen & disk, no filtering
#define LogS MultiLog(LogOn, LogOff)			// screen only
#define LogXY(x,y) MultiLog((x), (y))			// x and y are zero-based.

std::string getTimeStr();

