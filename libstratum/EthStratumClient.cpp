 
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
	GenericFarm<EthashProofOfWork>* f, 
	MinerType m, 
	string const & host, 
	string const & port,
	string const & password,
	int const & retries,
	int const & worktimeout
)
	: m_socket(m_io_service)
{
	// Note: host should not include the "http://" prefix.
	m_host = host;
	m_port = port;
	m_password = password;

	m_authorized = false;
	m_connected = false;
	m_running = true;
	m_failoverAvailable = retries != 0;
	m_maxRetries = m_failoverAvailable ? retries : c_StopWorkAt;
	m_retries = 0;
	m_worktimeout = worktimeout;

	p_farm = f;
	
	f->onSolutionFound([&] (EthashProofOfWork::Solution sol, int miner) {
		m_solutionMiner = miner;
		if (isConnected())
			submit(sol);
		else
			LogB << "Can't submit solution: Not connected";
		return false;
	});

	p_worktimer = nullptr;

	launchIOS();

}

EthStratumClient::~EthStratumClient()
{
	m_io_service.stop();
}

/*-----------------------------------------------------------------------------------
* launchIOS
*----------------------------------------------------------------------------------*/
void EthStratumClient::launchIOS()
{

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

void EthStratumClient::connectStratum()
{
	LogB << "Connecting to stratum server " << m_host + ":" + m_port << " ...";
	tcp::resolver resolver(m_io_service);
	tcp::resolver::query query(m_host, m_port);

	boost::system::error_code ec;
	connect(m_socket, resolver.resolve(query), ec);
	if (ec)
	{
		reconnect("Could not connect to stratum server " + m_host + ":" + m_port + " : " + ec.message());
		return;
	}
	m_connected = true;
	m_retries = 0;

	std::ostream os(&m_requestBuffer);
	os << "{\"id\": 1, \"method\": \"mining.subscribe\", \"params\": []}\n";
	writeStratum(m_requestBuffer);
	readline();
}


void EthStratumClient::disconnect()
{
	LogS << "Disconnecting from stratum server";
	m_connected = false;
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
		m_current.reset();
		p_farm->setWork(m_current);
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
	Json::Value error = responseObject.get("error", new Json::Value);
	if (error.isArray())
	{
		string msg = error.get(1, "Unknown error").asString();
		LogB << msg;
	}
	std::ostream os(&m_requestBuffer);
	Json::Value params;
	int id = responseObject.get("id", Json::Value::null).asInt();
	switch (id)
	{
		case 1:
		{
			LogB << "Connection established";
			params = responseObject.get("result", Json::Value::null);
			if (params.isArray())
				setWork(params);

			os << "{\"id\": 3, \"method\": \"mining.authorize\", \"params\": [\"\",\"" << m_password << "\"]}\n";
			writeStratum(m_requestBuffer);
		}
		break;
	case 2:
		// nothing to do...
		break;
	case 3:
		m_authorized = responseObject.get("result", Json::Value::null).asBool();
		if (!m_authorized)
		{
			LogB << "Stratum logon rejected. Worker not authorized.";
			if (!m_failoverAvailable)
				exit(-1);
			disconnect();
			return;
		}
		break;
	case 4:
		if (responseObject.get("result", false).asBool())
			p_farm->solutionFound(SolutionState::Accepted, m_stale, m_solutionMiner);
		else
			p_farm->solutionFound(SolutionState::Rejected, m_stale, m_solutionMiner);
		break;
	default:
		string method = responseObject.get("method", "").asString();

		if (method == "mining.notify")
		{
			params = responseObject.get("params", Json::Value::null);
			if (params.isArray())
				setWork(params);
		}
		else if (method == "client.get_version")
		{
			os << "{\"error\": null, \"id\" : " << id << ", \"result\" : \"" << ETH_PROJECT_VERSION << "\"}\n";
			writeStratum(m_requestBuffer);
		}
		break;
	}

}

void EthStratumClient::writeStratum(boost::asio::streambuf& buff)
{
	boost::system::error_code ec;
	string s = streamBufToStr(buff);
	LogF << "Stratum.Send: " << s;
	write(m_socket, buff, ec);
	if (ec)
	{
		LogB << "Error writing to stratum socket : " << ec.message();
		LogD << "  - was attempting to send : " << s;
		reconnect("");
	}
}

void EthStratumClient::setWork(Json::Value params)
{
	string sHeaderHash = params.get((Json::Value::ArrayIndex)1, "").asString();
	string sSeedHash = params.get((Json::Value::ArrayIndex)2, "").asString();
	string sShareTarget = params.get((Json::Value::ArrayIndex)3, "").asString();

	if (sHeaderHash != "" && sSeedHash != "" && sShareTarget != "")
	{
		h256 seedHash = h256(sSeedHash);
		h256 headerHash = h256(sHeaderHash);

		if (headerHash != m_current.headerHash)
		{
			if (p_worktimer)
				p_worktimer->cancel();

			m_previous.headerHash = m_current.headerHash;
			m_previous.seedHash = m_current.seedHash;
			m_previous.boundary = m_current.boundary;

			m_current.headerHash = headerHash;
			m_current.seedHash = seedHash;
			m_current.boundary = h256(sShareTarget);

			try
			{
				string sBlocknumber = params.get((Json::Value::ArrayIndex)4, "").asString();
				unsigned blockNum = (sBlocknumber == "") ? 0 : std::stoul(sBlocknumber, nullptr, 16);
				if (m_onWorkPackage)
					m_onWorkPackage(blockNum);
			}
			catch (const std::exception& e)
			{
				LogB << "Error in EthStratumClient::setWork. Unable to convert block number. " << e.what();
			}

			p_farm->setWork(m_current);
			p_worktimer = new boost::asio::deadline_timer(m_io_service, boost::posix_time::seconds(m_worktimeout));
			p_worktimer->async_wait(boost::bind(&EthStratumClient::work_timeout_handler, this, boost::asio::placeholders::error));
		}
	}
}


void EthStratumClient::work_timeout_handler(const boost::system::error_code& ec) {
	if (!ec) {
		LogB << "No new work received in " << m_worktimeout << " seconds.";
		reconnect("");
	}
}

bool EthStratumClient::submit(EthashProofOfWork::Solution solution) {
	EthashProofOfWork::WorkPackage tempWork(m_current);
	EthashProofOfWork::WorkPackage tempPreviousWork(m_previous);

	LogB << "Solution found; Submitting to " << m_host << "...";

	if (EthashAux::eval(tempWork.seedHash, tempWork.headerHash, solution.nonce).value < tempWork.boundary)
	{
		string json;

		json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"\",\"\",\"0x" + solution.nonce.hex() + "\",\"0x" + tempWork.headerHash.hex() + "\",\"0x" + solution.mixHash.hex() + "\"]}\n";
		std::ostream os(&m_requestBuffer);
		os << json;
		m_stale = false;
		writeStratum(m_requestBuffer);
		return true;
	}
	else if (EthashAux::eval(tempPreviousWork.seedHash, tempPreviousWork.headerHash, solution.nonce).value < tempPreviousWork.boundary)
	{
		string json;

		json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"\",\"\",\"0x" + solution.nonce.hex() + "\",\"0x" + tempPreviousWork.headerHash.hex() + "\",\"0x" + solution.mixHash.hex() + "\"]}\n";
		std::ostream os(&m_requestBuffer);
		os << json;
		m_stale = true;
		writeStratum(m_requestBuffer);
		return true;
	}
	else {
		m_stale = false;
		p_farm->solutionFound(SolutionState::Failed, NULL, m_solutionMiner);
	}

	return false;
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

