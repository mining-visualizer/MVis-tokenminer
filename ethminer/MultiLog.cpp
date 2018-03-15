
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

#include "MultiLog.h"
#include "Misc.h"
#include <chrono>
#include <iomanip> 

#if defined(WIN32)
#include <Windows.h>
#endif


using namespace std;
using namespace boost;

// this class supports regular streaming output, and positioned output

// when a log statement is filtered it is only output if some part of it matches one of the filters.

// static initializers
filesystem::path MultiLog::m_logFilename;
filesystem::path MultiLog::m_filterFilename;
std::vector<std::string> MultiLog::m_filters;
std::ofstream MultiLog::m_file;
SteadyClock::time_point MultiLog::m_filterCheckTime;
std::time_t MultiLog::m_filterChangeTime;
Mutex MultiLog::x_filter;
Mutex MultiLog::x_screenOutput;
Mutex MultiLog::x_diskOutput;

// this is the reference line used for positioned output. it points to the line after the 
// last line of scrolled output.
int MultiLog::m_currentYBase;

// this points to the line after the last line of positioned output
int MultiLog::m_currentYExtent;

// after every output record the current y position of the cursor.  at the next output, if the 
// cursor has moved we know a rogue agent has output something directly to screen.
int MultiLog::m_rogueCatcher = 0;


void MultiLog::Init()
{
	m_logFilename = getAppDataFolder() / "log.txt";
	m_filterFilename = getAppDataFolder() / "logfilters.txt";
	trimLogFile();
	try
	{
		m_file.open(m_logFilename.generic_string(), ofstream::app);
	}
	catch (const std::exception& e)
	{
		std::cout << "Exception: MultiLog.Init - " << e.what() << std::endl;
	}
	loadFilters();
	m_currentYBase = m_currentYExtent = getYPos();
}


MultiLog::MultiLog(LogMode _screenMode, LogMode _diskMode) 
	: m_screenMode(_screenMode), m_diskMode(_diskMode), m_positioned(false)
{
	// check if we need to reload the filters
	Guard l(x_filter);
	int sinceLastCheck = std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - m_filterCheckTime).count();
	if (sinceLastCheck > 10)
	{
		if (filesystem::exists(m_filterFilename))
		{
			// has it been modified since last check?
			std::time_t lastModified = filesystem::last_write_time(m_filterFilename);
			if (lastModified != m_filterChangeTime)
				loadFilters();
		}
		else
		{
			m_filters.clear();
			m_filterChangeTime = 0;
		}
		m_filterCheckTime = SteadyClock::now();
	}

}

// x and y are zero-based
MultiLog::MultiLog(int _xpos, int _ypos)
	: m_screenMode(LogOn), m_diskMode(LogOff), m_positioned(true), m_xpos(_xpos), m_ypos(_ypos) {}


MultiLog::~MultiLog()
{ 
	int filterResult = 999;
	string outStr = m_sstr.str();

	if (m_screenMode != LogOff)
		if (m_screenMode == LogOn || (1 == (filterResult = filterMatch(outStr))))
		{
			Guard l(x_screenOutput);
			int ypos = getYPos();
			if (ypos != m_rogueCatcher)
			{
				// start over after the rogue output
				m_currentYBase = m_currentYExtent = ypos;
			}
			if (m_positioned)
				GotoXY(m_xpos, m_currentYBase + m_ypos);
			else
			{
				if (m_currentYExtent > m_currentYBase)
					clearYExtent();

				GotoXY(0, m_currentYBase);
			}
			
			simpleDebugOut(cout, outStr, !m_positioned, !m_positioned);

			if (m_positioned)
			{
				// update the Y extent in case we've positioned something further down the screen.
				m_currentYExtent = max(m_currentYExtent, getYPos() + 1);
				// put cursor after the last positioned line in case a rogue agent outputs some text directly to screen.
				GotoXY(0, m_currentYExtent);
				// the previous goto might have caused a scroll
				m_currentYExtent = min(m_currentYExtent, getBufferHeight() - 1);
			}
			else
				m_currentYBase = m_currentYExtent = getYPos();

			m_rogueCatcher = m_currentYExtent;
		}

	if (m_diskMode != LogOff)
	{
		if (m_diskMode == LogFiltered && filterResult == 999)
			filterResult = filterMatch(outStr);
		if ((m_diskMode == LogOn || filterResult == 1) && m_file.is_open())
		{
			Guard l(x_diskOutput);
			simpleDebugOut(m_file, outStr, true, true);
		}
	}
}

int MultiLog::getXY(int& x, int& y)
{

#ifdef WIN32

	CONSOLE_SCREEN_BUFFER_INFO info;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
	x = info.dwCursorPosition.X;
	y = info.dwCursorPosition.Y;
	return 1;

#else

	// credits to Arnaud B 
	// https://stackoverflow.com/questions/16026858/reading-the-device-status-report-ansi-escape-sequence-reply/30698932#30698932

    fd_set readset;
    int success = 0;
    struct timeval time;
    struct termios term, initial_term;

    /*We store the actual properties of the input console and set it as:
    no buffered (~ICANON): avoid blocking 
    no echoing (~ECHO): do not display the result on the console*/
    tcgetattr(STDIN_FILENO, &initial_term);
    term = initial_term;
    term.c_lflag &=~ICANON;
    term.c_lflag &=~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);

    //We request position
	printf("\033[6n");
    fflush(stdout);

    //We wait 100ms for a terminal answer
    FD_ZERO(&readset);
    FD_SET(STDIN_FILENO, &readset);
    time.tv_sec = 0;
    time.tv_usec = 100000;

    //If it success we try to read the cursor value
    if (select(STDIN_FILENO + 1, &readset, NULL, NULL, &time) == 1) 
      if (scanf("\033[%d;%dR", &y, &x) == 2)
      {
	      success = 1;
	      y--;
	      x--;
      } 

    //We set back the properties of the terminal
    tcsetattr(STDIN_FILENO, TCSADRAIN, &initial_term);

    return success;

#endif

}


int MultiLog::getYPos()
{
	int x, y;
	getXY(x, y);
	return y;
}


int MultiLog::getXPos()
{
	int x, y;
	getXY(x, y);
	return x;
}

void MultiLog::GotoXY(int x, int y)
{

#ifdef WIN32

	COORD coord;
	CONSOLE_SCREEN_BUFFER_INFO info;
	CHAR_INFO fill;
	SMALL_RECT scrollRect;

	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
	// scroll if we need to
	fill.Char.AsciiChar = ' ';
	fill.Attributes = info.wAttributes;
	scrollRect.Top = 0;
	scrollRect.Left = 0;
	scrollRect.Bottom = info.dwSize.Y - 1;
	scrollRect.Right = info.dwSize.X - 1;
	coord.X = 0;
	// we're subtracting off an extra -1 because if we just scroll enough so we can write on the last line, it will
	// most likely have a linefeed at the end so then it will scroll again naturally, and our positioning will be off.
	coord.Y = min(0, info.dwSize.Y - 1 - y);
	ScrollConsoleScreenBuffer(GetStdHandle(STD_OUTPUT_HANDLE), &scrollRect, NULL, coord, &fill);

	// make adjustments for the amount that we scrolled.
	m_currentYBase += coord.Y;
	coord.X = x;
	coord.Y = y + coord.Y;
	SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);

#else

	int h = getBufferHeight();

	if (y >= h) 
	{
		// put the cursor on the last line
		printf("\033[%d;%dH",h, 1);
		while (y >= h) 
		{
			// scroll down by one
			printf("\033E");
			y--;
			m_currentYBase--;
		}
	}
	printf("\033[%d;%dH",y+1, x+1);

#endif

}

void MultiLog::trimLogFile()
{
	if (filesystem::exists(m_logFilename))
	{
		if (filesystem::file_size(m_logFilename) > 8000000)
		{
			string filename = m_logFilename.generic_string();
			ifstream file(filename);
			std::stringstream buffer;
			buffer << file.rdbuf();
			string s1 = buffer.str();
			file.close();

			ofstream file2(filename, ios::trunc | ios::out);
			file2 << s1.substr(s1.size() - 4000000);
			file2.close();
		}
	}
}

void MultiLog::loadFilters()
{
	try
	{
		if (filesystem::exists(m_filterFilename))
		{
			ifstream f;
			string s;
			m_filters.clear();
			f.open(m_filterFilename.generic_string(), fstream::in);
			while (getlineEx(f, s))
			{
				if (s != "")
					m_filters.push_back(LowerCase(s));
			}
			m_filterChangeTime = filesystem::last_write_time(m_filterFilename);
		}
	}
	catch (const std::exception& e)
	{
		std::cout << "Exception: MultiLog.loadFilters - " << e.what() << std::endl;
	}
}

int MultiLog::filterMatch(std::string _s)
{
	LowerCase(_s);
	for (auto& f : m_filters)
		if (_s.find(f) != string::npos)
			return 1;
	return 0;
}

void MultiLog::clearLine(int _line)
{
#ifdef WIN32

	// get buffer width
	CONSOLE_SCREEN_BUFFER_INFO info;
	COORD coord;
	DWORD charsWritten;
	coord.X = 0;
	coord.Y = _line;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
	FillConsoleOutputCharacter(GetStdHandle(STD_OUTPUT_HANDLE), ' ', info.dwSize.X, coord, &charsWritten);

#else

	GotoXY(1, _line);
	printf("\033[2K");

#endif
}


void MultiLog::clearYExtent()
{
	for (int i = m_currentYBase; i < m_currentYExtent; i++)
	{
		clearLine(i);
	}
}

int MultiLog::getBufferHeight()
{
#ifdef WIN32

	CONSOLE_SCREEN_BUFFER_INFO info;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
	return info.dwSize.Y;

#else

	struct winsize w;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	return w.ws_row;

#endif
}

void MultiLog::simpleDebugOut(ostream& _os, string const& _s, bool _time, bool _eol)
{
	if (_time)
		_os << getTimeStr();
	if (_eol)
		_os << _s << endl << flush;
	else
		_os << _s << flush;
}

std::string getTimeStr()
{
	std::stringstream s;
	time_t rawTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	unsigned ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 1000;
	char buf[27];
	if (strftime(buf, 27, "d%#d %X", localtime(&rawTime)) == 0)
		buf[0] = '\0'; // empty if case strftime fails
	s << buf << "." << setw(3) << setfill('0') << ms << "> ";
	return s.str();
}
