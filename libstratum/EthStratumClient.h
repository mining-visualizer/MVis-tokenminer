
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
#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <json/json.h>
#include <libdevcore/Log.h>
#include <libdevcore/FixedHash.h>
#include <libethcore/Farm.h>
#include <libethcore/EthashAux.h>
#include "BuildInfo.h"


using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;
using namespace dev;
using namespace dev::eth;


class EthStratumClient
{
public:

	using WorkPackageFn = std::function<void(unsigned int)>;

	EthStratumClient(
		string const & host, 
		string const & port,
		int const & retries,
		int const & worktimeout,
		string const & userAcct
	);
	~EthStratumClient();

	void restart();
	bool isRunning();
	bool isConnected();
	void submitWork(h256 _nonce, bytes _hash, bytes _challenge, uint64_t _difficulty);
	void getWork(bytes& _challenge, h256& _target, uint64_t& _difficulty, string& _hashingAcct);
	void disconnect();
	void switchAcct(string _newAcct);

private:
	void connectStratum();
	void launchIOS();
	string checkHost(string _host);
	void reconnect(string msg);
	void readline();
	void readResponse(const boost::system::error_code& ec, std::size_t bytes_transferred);
	void processReponse(Json::Value& responseObject);
	void writeStratum(Json::Value _json);
	void setWork(Json::Value params);
	void work_timeout_handler(const boost::system::error_code& ec);
	string streamBufToStr(boost::asio::streambuf &buff);
	void logJson(Json::Value _json);
	bool validInput(Json::Value _json);

	string m_host;
	string m_port;
	string m_password;

	bool m_authorized;	// we're subscribed to the pool
	bool m_connected;	// this refers to a TCP connection
	bool m_running;		// the Boost::Asio worker thread is running & listening

	int	m_retries = 0;
	int	m_maxRetries;
	int m_worktimeout;
	bool m_failoverAvailable;
	bool m_verbose = true;

	boost::asio::io_service m_io_service;
	tcp::socket m_socket;

	boost::asio::streambuf m_requestBuffer;
	boost::asio::streambuf m_responseBuffer;

	boost::asio::deadline_timer * p_worktimer;
	boost::asio::deadline_timer * p_reconnect;

	int m_solutionMiner;

	h256 m_target;
	bytes m_challenge;
	uint64_t m_difficulty;
	std::string m_hashingAcct;
	std::string m_userAcct;

	Mutex x_work;
};