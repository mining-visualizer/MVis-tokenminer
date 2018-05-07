 
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

#include "EthStratumClient.h"
#include <libdevcore/Log.h>
#include <libethash/endian.h>
using boost::asio::ip::tcp;

#define BOOST_ASIO_ENABLE_CANCELIO 

EthStratumClient::EthStratumClient(
	string const & host,
	string const & port,
	int const & retries,
	int const & worktimeout,
	string const & userAcct
)
	: m_socket(m_io_service)
{
	m_host = checkHost(host);
	m_port = port;
	m_userAcct = userAcct;

	m_authorized = false;
	m_connected = false;
	m_failoverAvailable = retries != 0;
	m_maxRetries = m_failoverAvailable ? retries : c_StopWorkAt;
	m_retries = 0;
	m_worktimeout = worktimeout;

	p_worktimer = nullptr;

	launchIOS();
}

EthStratumClient::~EthStratumClient()
{
	m_io_service.stop();
}

string EthStratumClient::checkHost(string _host)
{
	// boost asio doesn't like the http:// prefix (stratum mining)
	// json rpc (FarmClient) is ok with it though.

	size_t p;
	if ((p = _host.find("://")) != string::npos)
	{
		_host = _host.substr(p + 3, string::npos);
	}

	return _host;
}

/*-----------------------------------------------------------------------------------
* launchIOS
*----------------------------------------------------------------------------------*/
void EthStratumClient::launchIOS()
{

	m_running = true;
	connectStratum();

	boost::thread bt([&] () {

		while (m_running)
		{
			try
			{
				m_io_service.run();
				if (m_running)
					// this normally shouldn't happen.
					m_io_service.reset();
				else
					break;
			}
			catch (std::exception& e)
			{
				LogB << "EthStratumClient io_service exception : " << e.what();
				m_io_service.reset();
			}
		}
	});
}

void EthStratumClient::restart()
{
	launchIOS();
}

void EthStratumClient::connectStratum()
{
	if (m_verbose)
		LogB << "Connecting to stratum server " << m_host + ":" + m_port << " ...";
	tcp::resolver resolver(m_io_service);
	tcp::resolver::query query(m_host, m_port);

	boost::system::error_code ec;
	try
	{
		connect(m_socket, resolver.resolve(query), ec);
	}
	catch (const std::exception& e)
	{
		LogB << "Exception: EthStratumClient::connectStratum - " << e.what();
	}
	if (ec)
	{
		reconnect("Could not connect to stratum server " + m_host + ":" + m_port + " : " + ec.message());
		return;
	}
	m_connected = true;
	m_retries = 0;

	Json::Value msg;
	msg["id"] = 1;
	msg["method"] = "mining.subscribe";
	msg["params"].append(m_userAcct);
	writeStratum(msg);

	readline();
}


void EthStratumClient::disconnect()
{
	if (m_verbose)
		LogS << "Disconnecting from stratum server";
	m_connected = false;
	m_authorized = false;
	m_running = false;
	m_socket.close();
	m_io_service.stop();
}

void EthStratumClient::reconnect(string msg)
{
	if (msg != "")
		LogB << msg;

	m_retries++;
	if (m_retries == m_maxRetries)
	{
		// if there's a failover available, we'll switch to it, but worst case scenario, it could be 
		// unavailable as well, so at some point we should pause mining.  we'll do it here.
		//m_current.reset();
		//p_farm->setWork(m_current);
		LogB << "Mining paused ...";
		if (m_failoverAvailable)
		{
			disconnect();
			return;
		}
	}

	LogS << "Reconnecting in 5 seconds...";
	p_reconnect = new boost::asio::deadline_timer(m_io_service, boost::posix_time::seconds(5));
	p_reconnect->async_wait(boost::bind(&EthStratumClient::connectStratum, this));
	m_connected = false;
	m_socket.close();
}

void EthStratumClient::readline() {
	async_read_until(m_socket, m_responseBuffer, "\n",
					 boost::bind(&EthStratumClient::readResponse, this,
								 boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
}

void EthStratumClient::readResponse(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	dev::setThreadName("stratum");
	if (!ec && bytes_transferred)
	{
		std::istream is(&m_responseBuffer);
		std::string response;
		getline(is, response);
		LogF << "Stratum.Receive : " << response;

		if (!response.empty() && response.front() == '{' && response.back() == '}') 
		{
			Json::Value responseObject;
			Json::Reader reader;
			if (reader.parse(response.c_str(), responseObject))
				processReponse(responseObject);
			else
			{
				LogB << "Unable to parse JSON in EthStratumClient::readResponse : " << reader.getFormattedErrorMessages();
				LogB << "  - was attempting to parse : " << response;
			}
		}
		else
			LogB << "Invalid JSON response in EthStratumClient::readResponse : " << response;

		if (m_connected)
			readline();
	}
	else
	{
		if (m_running && m_connected)
			reconnect("Read response failed: " + ec.message());
	}
}


void EthStratumClient::processReponse(Json::Value& responseObject)
{
	if (!validInput(responseObject))
	{
		LogB << "Invalid JSON response from pool: " << responseObject.toStyledString();
		return;
	}	

	switch (responseObject["id"].asInt())
	{
		case 1:		// response from mining.subscribe
			
			if (responseObject["result"].asBool())
			{
				if (m_verbose)
					LogB << "Connection established";
				m_authorized = true;
			} 
			else
			{
				reconnect("Pool login rejected. Reason given : " + responseObject["error"][1].asString());
				return;
			}
			break;
		case 4:		// response from mining.submit
			if (!responseObject["result"].asBool())
			{
				LogB << "Solution was rejected by the pool. Reason : " << responseObject["error"][1].asString();
			}
			break;
		default:
			if (responseObject["method"].asString() == "mining.notify")
			{
				Guard l(x_work);
				m_challenge = fromHex(responseObject["params"][0].asString());
				m_target = u256(responseObject["params"][1].asString());
				m_difficulty = atoll(responseObject["params"][2].asString().c_str());
				m_hashingAcct = responseObject["params"][3].asString();
			} 
			else
			{
				LogB << "Unexpected JSON notification from pool : " << responseObject.toStyledString();
			}
			break;
	}
}

bool EthStratumClient::validInput(Json::Value _json)
{
	if (_json.isMember("result"))
	{
		if (!_json.isMember("error"))
			return false;

		if (!_json["result"].asBool() && (!_json["error"].isArray() || _json["error"].size() < 2))
			return false;

		return true;
	} 
	else if (_json.isMember("method"))
	{
		if (!_json.isMember("params") || !_json["params"].isArray())
			return false;

		return true;
	} 
	else
	{
		return false;
	}
}

void EthStratumClient::writeStratum(Json::Value _json)
{
	Json::FastWriter fw;
	std::string msg = fw.write(_json);
	LogF << "Stratum.Send : " << msg;
	std::ostream os(&m_requestBuffer);
	os << msg;
	boost::system::error_code ec;
	write(m_socket, m_requestBuffer, ec);
	if (ec)
	{
		LogB << "Error writing to stratum socket : " << ec.message();
		LogD << "  - was attempting to send : " << msg;
		reconnect("");
	}
}


void EthStratumClient::work_timeout_handler(const boost::system::error_code& ec) {
	if (!ec) {
		LogB << "No new work received in " << m_worktimeout << " seconds.";
		reconnect("");
	}
}

void EthStratumClient::submitWork(h256 _nonce, bytes _hash, bytes _challenge, uint64_t _difficulty) {

	Json::Value msg;

	msg["id"] = 4;
	msg["method"] = "mining.submit";
	msg["params"].append("0x" + _nonce.hex());
	msg["params"].append(m_userAcct);
	msg["params"].append("0x" + toHex(_hash));
	msg["params"].append((Json::UInt64)_difficulty);
	msg["params"].append("0x" + toHex(_challenge));
	writeStratum(msg);
}

string EthStratumClient::streamBufToStr(boost::asio::streambuf& buff)
{
	boost::asio::streambuf::const_buffers_type bufs = buff.data();
	std::string str(boost::asio::buffers_begin(bufs), boost::asio::buffers_begin(bufs) + buff.size());
	return str;
}

void EthStratumClient::logJson(Json::Value _json)
{
	Json::FastWriter fw;
	fw.omitEndingLineFeed();
	LogF << "Stratum.Recv: " << fw.write(_json);
}

void EthStratumClient::getWork(bytes& _challenge, h256& _target, uint64_t& _difficulty, string& _hashingAcct)
{
	Guard l(x_work);
	_challenge = m_challenge;
	_target = m_target;
	_difficulty = m_difficulty;
	_hashingAcct = m_hashingAcct;
}

bool EthStratumClient::isRunning() 
{ 
	return m_running; 
}

bool EthStratumClient::isConnected() 
{ 
	return m_connected && m_authorized; 
}

void EthStratumClient::switchAcct(string _newAcct)
{
	m_verbose = false;
	disconnect();
	m_userAcct = _newAcct;
	this_thread::sleep_for(chrono::milliseconds(50));
	restart();
	this_thread::sleep_for(chrono::milliseconds(50));
	m_verbose = true;
}
