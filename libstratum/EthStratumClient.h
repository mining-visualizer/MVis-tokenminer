
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
		GenericFarm<EthashProofOfWork> * f, 
		MinerType m, 
		string const & host, 
		string const & port,
		string const & password,
		int const & retries,
		int const & worktimeout
	);
	~EthStratumClient();

	bool isRunning() { return m_running; }
	bool isConnected() { return m_connected && m_authorized; }
	h256 currentHeaderHash() { return m_current.headerHash; }
	bool current() { return m_current; }
	bool submit(EthashProofOfWork::Solution solution);
	void disconnect();
	void onWorkPackage(WorkPackageFn const& handler) { m_onWorkPackage = handler; }

private:
	void connectStratum();
	void launchIOS();
	void reconnect(string msg);
	void readline();
	void readResponse(const boost::system::error_code& ec, std::size_t bytes_transferred);
	void processReponse(Json::Value& responseObject);
	void writeStratum(boost::asio::streambuf &buff);
	void setWork(Json::Value params);
	void work_timeout_handler(const boost::system::error_code& ec);
	string streamBufToStr(boost::asio::streambuf &buff);
	void logJson(Json::Value _json);

	string m_host;
	string m_port;
	string m_password;

	bool m_authorized;
	bool m_connected;	// this refers to a TCP connection
	bool m_running;

	int	m_retries = 0;
	int	m_maxRetries;
	int m_worktimeout;
	bool m_failoverAvailable;

	GenericFarm<EthashProofOfWork> * p_farm;
	EthashProofOfWork::WorkPackage m_current;
	EthashProofOfWork::WorkPackage m_previous;
	WorkPackageFn m_onWorkPackage;

	bool m_stale = false;

	boost::asio::io_service m_io_service;
	tcp::socket m_socket;

	boost::asio::streambuf m_requestBuffer;
	boost::asio::streambuf m_responseBuffer;

	boost::asio::deadline_timer * p_worktimer;
	boost::asio::deadline_timer * p_reconnect;

	double m_nextWorkDifficulty;
	int m_solutionMiner;

};