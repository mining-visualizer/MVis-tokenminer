
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

#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <libdevcore/Guards.h>

using namespace dev;

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;


// this implements a periodic timer / callback function.  be sure to call Init first.
class TimerCallback
{
public:

	using CallbackFn = std::function<void(void*)>;

	static void Init();

	TimerCallback(CallbackFn _cb);
	// rate is milliseconds
	void start(long _rate);
	void stop();
	void callback(const boost::system::error_code& e);
	void setData(void* _data);

public:
	bool isRunning;

private:

	static boost::thread* m_bt;
	static boost::asio::io_service* m_ios;
	static boost::asio::io_service::work* m_work;

	boost::asio::deadline_timer* m_timer;
	CallbackFn m_cb;
	long m_rate;
	void* m_data;

};	// class TimerCallback


#ifdef _WIN32

class Timer
{
public:

	Timer()
	{
		QueryPerformanceFrequency(&m_frequency);
		restart();
	}

	void restart()
	{
		QueryPerformanceCounter(&m_startTime);
	}

	double elapsedSeconds() const
	{
		LARGE_INTEGER endingTime;
		QueryPerformanceCounter(&endingTime);
		return (double)(endingTime.QuadPart - m_startTime.QuadPart) / m_frequency.QuadPart;
	}

	uint64_t elapsedMilliseconds() const
	{
		LARGE_INTEGER endingTime;
		QueryPerformanceCounter(&endingTime);
		return (endingTime.QuadPart - m_startTime.QuadPart) * 1000 / m_frequency.QuadPart;
	}

	uint64_t elapsedMicroseconds() const
	{
		LARGE_INTEGER endingTime;
		QueryPerformanceCounter(&endingTime);
		return (endingTime.QuadPart - m_startTime.QuadPart) * 1000000 / m_frequency.QuadPart;
	}

private:
	LARGE_INTEGER m_startTime;
	LARGE_INTEGER m_frequency;
};

#else

class Timer
{
public:
	Timer() 
	{ 
		restart(); 
	}

	void restart() 
	{ 
		m_t = std::chrono::high_resolution_clock::now(); 
	}

	double elapsedSeconds() const
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - m_t).count() / 1000000.0;
	}

	uint64_t elapsedMilliseconds() const
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - m_t).count();
	}

	uint64_t elapsedMicroseconds() const
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - m_t).count();
	}

private:
	std::chrono::high_resolution_clock::time_point m_t;
};

#endif


int strToInt(std::string s, int defaultVal);
bool isDigits(const std::string &_str);
bool isNumeric(const std::string &_str);