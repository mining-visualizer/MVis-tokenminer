
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

#include "Common.h"
#include "MultiLog.h"

using namespace std;
using namespace boost;


// static initializers
boost::thread* TimerCallback::m_bt = nullptr;
boost::asio::io_service* TimerCallback::m_ios = nullptr;
boost::asio::io_service::work* TimerCallback::m_work;

/*-----------------------------------------------------------------------------------
* class TimerCallback
*----------------------------------------------------------------------------------*/
void TimerCallback::Init()
{
	// this needs to be called once before any timer callbacks are instantiated.
	static Mutex x_init;
	Guard l(x_init);

	// check if we've been called already.
	if (m_bt) return;

	// launch a thread to handle timer callbacks.
	m_ios = new boost::asio::io_service;
	m_bt = new boost::thread([&] () {
		for (;;)
		{
			try
			{
				m_work = new asio::io_service::work(*m_ios);
				m_ios->run();
				break;
			}
			catch (std::exception& e)
			{
				LogB << "TimerCallback::Init io_service exception : " << e.what();
				m_ios->reset();
			}
		}
	});
}

TimerCallback::TimerCallback(CallbackFn _cb) : isRunning(false), m_cb(_cb)
{
	if (!m_ios) 
		throw std::runtime_error("TimerCallback.constructor - boost::io_service has not been initialized");
	m_timer = new asio::deadline_timer(*m_ios);
}

// rate is milliseconds
void TimerCallback::start(long _rate)
{
	isRunning = true;
	m_rate = _rate;
	m_timer->expires_from_now(posix_time::milliseconds(m_rate));
	m_timer->async_wait(bind(&TimerCallback::callback, this, asio::placeholders::error));
}

void TimerCallback::stop()
{
	isRunning = false;
	m_timer->cancel();
}

void TimerCallback::callback(const boost::system::error_code& e)
{
	if (e == asio::error::operation_aborted)
	{
		isRunning = false;
		return;
	}
	m_timer->expires_at(m_timer->expires_at() + posix_time::milliseconds(m_rate));
	m_timer->async_wait(bind(&TimerCallback::callback, this, asio::placeholders::error));
	m_cb(m_data);
}

void TimerCallback::setData(void* _data) 
{ 
	m_data = _data; 
}


int strToInt(std::string s, int defaultVal)
{
	try
	{
		return std::stoi(s);
	}
	catch (...)
	{
		return defaultVal;
	}
}

bool isDigits(const std::string &_str)
{
	return std::all_of(_str.begin(), _str.end(), ::isdigit);
}

bool isNumeric(const std::string &_str)
{
	if (_str.empty())
		return false;
	char* p;
	double converted = strtod(_str.c_str(), &p);
	return !(*p);
}