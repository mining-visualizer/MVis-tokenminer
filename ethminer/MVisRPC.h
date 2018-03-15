
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

#include "UDPSocket.h"

namespace dev {
//namespace rpc {

using namespace std;

// notification rates
#define RATE_OFF			-3
#define RATE_ONE_TIME		-2
#define RATE_ON_CHANGE		-1
#define RATE_REGULAR		1	// really, anything > 0  (milliseconds)

#define MINER_RPC_VERSION	10


// this class implements a UDP JSON RPC interface between this program and MVis.

class MVisRPC
{
public:

	// if the client doesn't send us periodic keep-alive messages, we will
	// eventually assume it has gone away, and stop sending it anything.
	const int KeepAliveWait = 1000 * 60 * 2;

	/*-----------------------------------------------------------------------------------
	* constructor
	*----------------------------------------------------------------------------------*/
	MVisRPC(dev::eth::GenericFarm<dev::eth::EthashProofOfWork>& _f)
		: m_farm(&_f)
    {
		// set up for JSON RPC in case we need to talk to geth.
		LogF << "Trace: MVisRPC.constructor";
		m_nodeRpc = nullptr;

		int udpListen = atoi(ProgOpt::Get("Network", "UdpListen", "5225").c_str());
		if (udpListen == 0)
			LogB << "ERROR! Bad value for 'UdpListen' in .ini file.";
		udp = new UDPSocket(udpListen);
		// set up a handler for incoming UDP commands.
		udp->onCommandRecv(boost::bind(&MVisRPC::processCmd, this, _1));

		TimerCallback::Init();
		m_hashRateTimer = new TimerCallback(bind(&MVisRPC::hashRateCB, this, _1));
		m_hashSamplesTimer = new TimerCallback(bind(&MVisRPC::hashSamplesCB, this, _1));
		m_gpuTempsTimer = new TimerCallback(bind(&MVisRPC::gpuTempsCB, this, _1));
		m_fanSpeedsTimer = new TimerCallback(bind(&MVisRPC::fanSpeedsCB, this, _1));
		m_peerCountTimer = new TimerCallback(bind(&MVisRPC::peerCountCB, this, _1));
		m_acctBalanceTimer = new TimerCallback(bind(&MVisRPC::acctBalanceCB, this, _1));

		m_farm->onBestHash(boost::bind(&MVisRPC::onBestHash, this, _1));
		m_farm->onCloseHit(boost::bind(&MVisRPC::onCloseHit, this, _1, _2, _3));
		m_farm->onHashFault(boost::bind(&MVisRPC::onHashFault, this, _1));
		m_farm->onSetWork(boost::bind(&MVisRPC::onSetWork, this, _1));
		m_farm->onSolutionProcessed(boost::bind(&MVisRPC::onSolutionProcessed, this, _1, _2, _3, _4));

		m_keepAlive = new TimerCallback(bind(&MVisRPC::onKeepAliveExpired, this));
	}

	/*-----------------------------------------------------------------------------------
	* configNodeRPC.
	*----------------------------------------------------------------------------------*/
	void configNodeRPC(string const& _nodeURL)
	{
		m_client = new jsonrpc::HttpClient(_nodeURL);
		//m_nodeRpc = new ::FarmClient(*m_client);
	}

	/*-----------------------------------------------------------------------------------
	* processCmd.
	*   - event handler for incoming udp from the GUI app.
	*----------------------------------------------------------------------------------*/
	void processCmd(Json::Value const& cmd) 
	{
		Json::Value jsonResults(Json::objectValue);

		Json::FastWriter fw;
		fw.omitEndingLineFeed();
		LogF << "Trace: MVisRPC.processCmd. JSON = " << fw.write(cmd);
		jsonResults["id"] = cmd["id"];
		jsonResults["type"] = "response";
		jsonResults["data_id"] = cmd["command"];

		try
		{

			// --- connect --- 

			if (cmd["command"] == "connect")
			{
				if (!m_nodeRpc)
				{
					jsonResults["error"] = "Unable to establish a connection at this time. Internal configuration is not complete.";
					udp->send_packet(jsonResults, cmd["return_port"].asInt());
					return;
				}

				if (!(cmd.isMember("return_port") && cmd.isMember("miner_id") && cmd.isMember("rpc_version") && cmd.isMember("password")))
				{
					if (cmd.isMember("return_port"))
					{
						jsonResults["error"] = "Invalid arguments : expecting 'return_port', 'miner_id', 'password' & 'rpc_version'";
						udp->send_packet(jsonResults, cmd["return_port"].asInt());
					}
					return;
				}

				// if we already have a connection, reject this one, unless it is the same client
				if (udp->connected() && !udp->isCallerClient(cmd["return_port"].asInt()))
				{
					jsonResults["error"] = "Unable to establish a connection at this time. Already connected to another instance of MVis.";
					udp->send_packet(jsonResults, cmd["return_port"].asInt());
					return;
				}

				udp->setCallerAsClient(cmd["return_port"].asInt(), cmd["miner_id"].asInt());

				if (cmd["rpc_version"].asInt() != MINER_RPC_VERSION)
					throw std::invalid_argument("Miner RPC Error : Invalid RPC version number");

				if (cmd["password"].asString() != ProgOpt::Get("Network", "UdpPassword"))
					throw std::invalid_argument("Miner RPC Error : Password not accepted");

				m_keepAlive->start(KeepAliveWait);

				// send back a few vital pieces of information
				jsonResults["gpu_count"] = m_farm->minerCount();
				jsonResults["last_solution"] = m_farm->logger.retrieveLastSolution();
				jsonResults["solutions"] = m_farm->logger.solutionCount();
				jsonResults["close_hits"] = m_farm->logger.closeHitCount();
				jsonResults["hash_faults"] = m_farm->logger.hashFaultCount();
				jsonResults["boundary"] = (Json::UInt64) upper64OfHash(m_farm->boundary());
				jsonResults["best_hash"] = (Json::UInt64) m_farm->bestHash();
				jsonResults["best_hash_date"] = m_farm->logger.retrieveBestHashDate();
			}

			// --- ping ---

			else if (cmd["command"] == "ping")
			{
				udp->send_packet(jsonResults, cmd["return_port"].asInt());
				// we've already sent the response
				jsonResults = Json::nullValue;
			}

			// --- keep_alive ---

			else if (cmd["command"] == "keep_alive")
			{
				if (udp->connected())
					m_keepAlive->start(KeepAliveWait);
				else
				{
					LogS << "MVisRPC.processCmd : Unexpected keep_alive. Miner is disconnected.";
					// don't send a response
					jsonResults = Json::nullValue;
				}
			}

			// --- best hash ---

			else if (cmd["command"] == "best_hash")
			{
				if (!cmd.isMember("rate"))
					throw std::invalid_argument("Invalid arguments : expecting 'rate'");

				if (cmd["rate"] == RATE_OFF)
					m_reportBestHash = false;
				else if (cmd["rate"] == RATE_ONE_TIME || cmd["rate"] == RATE_ON_CHANGE)
				{
					// return current best hash values for both one-time and on-change requests.
					jsonResults["best_hash"] = (Json::UInt64)m_farm->bestHash();
					jsonResults["best_hash_date"] = m_farm->logger.retrieveBestHashDate();

					if (cmd["rate"] == RATE_ON_CHANGE)
						m_reportBestHash = true;
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF, ONE_TIME, or ON_CHANGE");
			}

			// --- retrieve close hits ---

			else if (cmd["command"] == "close_hits")
			{
				Json::Value data = Json::Value(Json::arrayValue);
				jsonResults["data"] = data;

				Json::Value closeHits = m_farm->logger.retrieveCloseHits(true);
				if (closeHits.size() > 0)
				{
					for (unsigned i = 0; i < closeHits.size(); )
					{
						data.append(closeHits[i]);
						if (++i % 20 == 0)
						{
							// send in batches of 20
							jsonResults["data"] = data;
							udp->send_packet(jsonResults);
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							data = Json::Value(Json::arrayValue);
						}
					}

					if (data.size() == 0)
						// set a flag indicating the json response has already been sent
						jsonResults = Json::nullValue;
					else
						jsonResults["data"] = data;
				}
			}

			// --- retrieve hash faults ---

			else if (cmd["command"] == "hash_faults")
			{
				Json::Value data = Json::Value(Json::arrayValue);
				jsonResults["data"] = data;

				Json::Value hashFaults = m_farm->logger.retrieveHashFaults(true);
				if (hashFaults.size() > 0)
				{
					for (unsigned i = 0; i < hashFaults.size(); )
					{
						data.append(hashFaults[i]);
						if (++i % 20 == 0)
						{
							// send in batches of 20
							jsonResults["data"] = data;
							udp->send_packet(jsonResults);
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							data = Json::Value(Json::arrayValue);
						}
					}

					if (data.size() == 0)
						// set a flag indicating the json response has already been sent
						jsonResults = Json::nullValue;
					else
						jsonResults["data"] = data;
				}
			}

			// --- retrieve solutions ---

			else if (cmd["command"] == "solutions")
			{
				Json::Value data = Json::Value(Json::arrayValue);
				jsonResults["data"] = data;

				Json::Value solutions = m_farm->logger.retrieveSolutions(true);
				if (solutions.size() > 0)
				{
					for (unsigned i = 0; i < solutions.size(); )
					{
						data.append(solutions[i]);
						if (++i % 20 == 0)
						{
							// send in batches of 20
							jsonResults["data"] = data;
							udp->send_packet(jsonResults);
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							data = Json::Value(Json::arrayValue);
						}
					}
					if (data.size() == 0)
						// set a flag indicating the json response has already been sent
						jsonResults = Json::nullValue;
					else
						jsonResults["data"] = data;
				}
			}

			// --- set close hit threshold ---

			else if (cmd["command"] == "close_hit_threshold")
			{
				if (!(cmd.isMember("close_hit_threshold") && cmd.isMember("work_unit_frequency")))
					throw std::invalid_argument("Invalid arguments : expecting 'close_hit_threshold' & 'work_unit_frequency'");
				
				// close_hit_threshold is expressed as a literal hash value
				// work_unit is expressed as the desired number of seconds between close hits.

				m_farm->setCloseHitThresholds(std::stoull(cmd["close_hit_threshold"].asString()), std::stoull(cmd["work_unit_frequency"].asString()));
			}

			// --- reset best hash ---

			else if (cmd["command"] == "reset_best_hash")
			{
				m_farm->resetBestHash();
			}

			// --- work package ---

			else if (cmd["command"] == "work_package")
			{
				if (!cmd.isMember("rate"))
					throw std::invalid_argument("Invalid arguments : expecting 'rate'");

				if (cmd["rate"] == RATE_OFF) 
					m_reportWorkPackage = false;
				else if (cmd["rate"] == RATE_ONE_TIME || cmd["rate"] == RATE_ON_CHANGE)
				{
					Json::Value data(Json::objectValue);
					data["blocknumber"] = m_farm->currentBlock;
					data["boundary"] = (Json::UInt64) upper64OfHash(m_farm->boundary());
					jsonResults["data"] = data;
					if (cmd["rate"] == RATE_ON_CHANGE)
						m_reportWorkPackage = true;
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF, ONE_TIME, or ON_CHANGE");
			}

			// --- hash rates ---

			else if (cmd["command"] == "hash_rates")
			{
				if (!(cmd.isMember("rate") && cmd.isMember("delta")))
					throw std::invalid_argument("Invalid arguments : expecting 'rate' & 'delta'");

				if (cmd["rate"] == RATE_OFF)
					m_hashRateTimer->stop();
				else if (cmd["rate"] == RATE_ONE_TIME || cmd["rate"] >= RATE_REGULAR)
				{
					// return the current hash rates in either case.
					assembleHashRates(jsonResults, 0.0);
					if (cmd["rate"] >= RATE_REGULAR)
					{
						m_hashRateTimer->setData((void*) uint64_t(cmd["delta"].asDouble() * 1000));
						m_hashRateTimer->start(cmd["rate"].asInt());			// rate is milliseconds
					}
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF, ONE_TIME, or REGULAR");
			}

			// --- hash samples ---

			else if (cmd["command"] == "hash_samples")
			{
				if (!(cmd.isMember("rate") && cmd.isMember("gpu")))
					throw std::invalid_argument("Invalid arguments : expecting 'rate' & 'gpu'");
				int miner = cmd["gpu"].asInt();
				if (miner >= m_farm->minerCount())
					throw std::invalid_argument("Invalid gpu number");
				
				if (cmd["rate"] == RATE_OFF)
					m_hashSamplesTimer->stop();
				else if (cmd["rate"] >= RATE_REGULAR)
				{
					m_hashSamplesTimer->setData((void*) miner);
					m_hashSamplesTimer->start(cmd["rate"].asInt());			// rate is milliseconds
					m_hashSampleStop = SteadyClock::now() + std::chrono::duration<int>(12);
					jsonResults["data"] = (Json::UInt64) m_farm->currentHash(miner);
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF or REGULAR");
			}

			// --- peer count ---

			else if (cmd["command"] == "peer_count")
			{
				if (!(cmd.isMember("rate") && cmd.isMember("delta")))
					throw std::invalid_argument("Invalid arguments : expecting 'rate' & 'delta'");

				if (cmd["rate"] == RATE_OFF)
					m_peerCountTimer->stop();
				else if (cmd["rate"] == RATE_ONE_TIME || cmd["rate"] >= RATE_REGULAR)
				{
					// return the current peer count in either case.
					Json::Value p(Json::nullValue);
					{
						RecursiveGuard l(x_rpc);
						Json::Value result = m_nodeRpc->CallMethod("net_peerCount", p);
						jsonResults["data"] = jsToInt(result.asString());
					}
					if (cmd["rate"] >= RATE_REGULAR)
					{
						m_peerCountTimer->setData((void*) cmd["delta"].asInt());
						m_peerCountTimer->start(cmd["rate"].asInt());			// rate is milliseconds
					}
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF, ONE_TIME, or REGULAR");
			}

			// --- account balance ---

			else if (cmd["command"] == "account_balance")
			{
				if (!cmd.isMember("rate") && cmd.isMember("delta"))
					throw std::invalid_argument("Invalid arguments : expecting 'rate' & 'delta'");

				if (cmd["rate"] == RATE_OFF)
					m_acctBalanceTimer->stop();
				else if (cmd["rate"] == RATE_ONE_TIME || cmd["rate"] >= RATE_REGULAR)
				{
					queryAcctBalance(jsonResults, 0.0);
					if (cmd["rate"] >= RATE_REGULAR)
					{
						m_acctBalanceTimer->setData((void*) int(cmd["delta"].asDouble() * 10000));
						m_acctBalanceTimer->start(cmd["rate"].asInt());			// rate is milliseconds
					}
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF, ONE_TIME, or REGULAR");
			}

			// --- gpuTemps ---

			else if (cmd["command"] == "gpu_temps")
			{
				if (!cmd.isMember("rate") && cmd.isMember("delta"))
					throw std::invalid_argument("Invalid arguments : expecting 'rate' & 'delta'");

				if (cmd["rate"] == RATE_OFF)
					m_gpuTempsTimer->stop();
				else if (cmd["rate"] == RATE_ONE_TIME || cmd["rate"] >= RATE_REGULAR)
				{
					assembleGPUTemps(jsonResults, 0.0);
					if (cmd["rate"] >= RATE_REGULAR)
					{
						m_gpuTempsTimer->setData((void*) int(cmd["delta"].asDouble() * 100));
						m_gpuTempsTimer->start(cmd["rate"].asInt());			// rate is milliseconds
					}
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF, ONE_TIME, or REGULAR");
			}

			// --- fan speeds ---

			else if (cmd["command"] == "fan_speeds")
			{
				if (!cmd.isMember("rate") && cmd.isMember("delta"))
					throw std::invalid_argument("Invalid arguments : expecting 'rate' & 'delta'");

				if (cmd["rate"] == RATE_OFF)
					m_fanSpeedsTimer->stop();
				else if (cmd["rate"] == RATE_ONE_TIME || cmd["rate"] >= RATE_REGULAR)
				{
					assembleFanSpeeds(jsonResults, 0);
					if (cmd["rate"] >= RATE_REGULAR)
					{
						m_fanSpeedsTimer->setData((void*) cmd["delta"].asInt());
						m_fanSpeedsTimer->start(cmd["rate"].asInt());			// rate is milliseconds
					}
				}
				else
					throw std::out_of_range("Invalid value for 'rate' : expecting OFF, ONE_TIME, or REGULAR");
			}

			// --- miner_count ---

			else if (cmd["command"] == "miner_count")
				jsonResults["data"] = m_farm->minerCount();

			// --- gpu_throttle ---

			else if (cmd["command"] == "gpu_throttle")
			{
				if (!cmd.isMember("gpu") && cmd.isMember("percent"))
					throw std::invalid_argument("Invalid arguments : expecting 'gpu' & 'percent'");
				int throttle = cmd["percent"].asInt();
				if (throttle < 0 || throttle > 100)
					throw std::invalid_argument("Invalid arguments : 'percent' should be between 0 - 100");
				LogF << "Throttle: MVisRPC gpu_throttle, percent = " << throttle;
				m_farm->setGpuThrottle(cmd["gpu"].asInt(), throttle);
			}

			// --- thermal_protection ---

			else if (cmd["command"] == "thermal_protection")
			{
				if (!(cmd.isMember("never_exceed") && cmd.isMember("safety_shutdown")))
					throw std::invalid_argument("Invalid arguments : expecting 'never_exceed' & 'safety_shutdown'");
				m_farm->thermalProtection(cmd["never_exceed"].asInt(), cmd["safety_shutdown"].asInt());
			}

			// --- pid_controller_tuning ---

			else if (cmd["command"] == "pid_controller_tuning")
			{
				if (!(cmd.isMember("gpu") && cmd.isMember("kp") && cmd.isMember("ki") && cmd.isMember("kd")))
					throw std::invalid_argument("Invalid arguments : expecting 'gpu', 'kp', 'ki' & 'kd'");
				m_farm->tunePIDController(cmd["gpu"].asInt(), cmd["kp"].asDouble(), cmd["ki"].asDouble(), cmd["kd"].asDouble());
			}

			// --- disconnect ---

			else if (cmd["command"] == "disconnect")
			{
				udp->send_packet(jsonResults);
				disconnect("");
				// set a flag indicating the json response has already been sent
				jsonResults = Json::nullValue;
			}
			else
				jsonResults["error"] = "Invalid command";

		}
		catch (std::exception& e)
		{
			jsonResults["error"] = e.what();
			LogB << "MVisRPC.ProcessCmd : " << e.what();
		}

		if (jsonResults != Json::nullValue)
			udp->send_packet(jsonResults);
	}

	/*-----------------------------------------------------------------------------------
	* disconnect
	*----------------------------------------------------------------------------------*/
	void disconnect(std::string _returnType)
	{
		LogF << "Trace: MVisRPC.disconnect";
		m_reportBestHash = false;
		m_reportWorkPackage = false;
		m_hashRateTimer->stop();
		m_hashSamplesTimer->stop();
		m_gpuTempsTimer->stop();
		m_fanSpeedsTimer->stop();
		m_peerCountTimer->stop();
		m_acctBalanceTimer->stop();
		if (udp->connected())
			udp->disconnect(_returnType);
		m_keepAlive->stop();
	}

	/*-----------------------------------------------------------------------------------
	* onBestHash
	*----------------------------------------------------------------------------------*/
	void onBestHash(uint64_t _bh)
	{
		if (m_reportBestHash)
		{
			Json::Value v(Json::objectValue);
			v["data_id"] = "best_hash";
			v["best_hash"] = (Json::UInt64)_bh;
			v["best_hash_date"] = m_farm->logger.now();
			v["type"] = "notify";
			udp->send_packet(v);
		}
	}

	/*-----------------------------------------------------------------------------------
	* onCloseHit
	*----------------------------------------------------------------------------------*/
	bool onCloseHit(uint64_t _closeHit, unsigned _work, int _miner)
	{

		static Mutex s_lock;
		Guard l(s_lock);
		if (udp->connected())
		{
			Json::Value closeHit;
			closeHit["date"] = m_farm->logger.now();
			closeHit["close_hit"] = (Json::UInt64)_closeHit;
			closeHit["work"] = _work;
			closeHit["gpu_miner"] = _miner;
			Json::Value v(Json::objectValue);
			v["data_id"] = "close_hits";
			// we're putting it into an array because it's convenient for the client to receive
			// real time close hits in the same format as historical close hits.
			v["data"][0] = closeHit;
			v["type"] = "notify";
			udp->send_packet(v);
			return true;
		}
		else
			return false;
	}

	/*-----------------------------------------------------------------------------------
	* onHashFault
	*----------------------------------------------------------------------------------*/
	bool onHashFault(int _miner)
	{

		static Mutex s_lock;
		Guard l(s_lock);
		if (udp->connected())
		{
			Json::Value hashFault;
			hashFault["date"] = m_farm->logger.now();
			hashFault["gpu_miner"] = _miner;
			Json::Value v(Json::objectValue);
			v["data_id"] = "hash_faults";
			// we're putting it into an array because it's convenient for the client to receive
			// real time hash faults in the same format as historical ones.
			v["data"][0] = hashFault;
			v["type"] = "notify";
			udp->send_packet(v);
			return true;
		}
		else
			return false;
	}

	/*-----------------------------------------------------------------------------------
	* onSolutionProcessed
	*----------------------------------------------------------------------------------*/
	bool onSolutionProcessed(unsigned _blockNumber, SolutionState _state, bool _stale, int _miner)
	{
		if (udp->connected())
		{
			Json::Value solution(Json::objectValue);
			solution["block"] = _blockNumber;
			solution["date"] = m_farm->logger.now();
			solution["gpu_miner"] = _miner;
			solution["state"] = _state;
			solution["stale"] = _stale;
			Json::Value packet(Json::objectValue);
			packet["data_id"] = "solutions";
			// we're putting it into an array because it's convenient for the client to receive
			// real time solutions in the same format as historical ones.
			packet["data"][0] = solution;
			packet["type"] = "notify";
			udp->send_packet(packet);
			return true;
		}
		else
			return false;
	}


	/*-----------------------------------------------------------------------------------
	* onSetWork
	*----------------------------------------------------------------------------------*/
	void onSetWork(uint64_t boundary) 
	{
		if (m_reportWorkPackage)
		{
			Json::Value v(Json::objectValue);
			v["data_id"] = "work_package";
			try
			{
				Json::Value data(Json::objectValue);
				data["blocknumber"] = m_farm->currentBlock;
				data["boundary"] = (Json::UInt64) boundary;
				v["data"] = data;
				v["type"] = "notify";
			}
			catch (std::exception& e)
			{
				v["error"] = e.what();
			}
			udp->send_packet(v);
		}
	}

	/*-----------------------------------------------------------------------------------
	* getBlockNumber
	*----------------------------------------------------------------------------------*/
	unsigned getBlockNumber() 
	{
		// caller is responsible for any exceptions
		RecursiveGuard l(x_rpc);
		LogF << "Trace: MVisRPC.getBlockNumber";
		Json::Value p;
		p = Json::nullValue;
		Json::Value result = m_nodeRpc->CallMethod("eth_blockNumber", p);
		return jsToInt(result.asString());
	}

	/*-----------------------------------------------------------------------------------
	* hashSamplesCB
	*----------------------------------------------------------------------------------*/
	void hashSamplesCB(void* _miner)
	{
		Json::Value v(Json::objectValue);
		v["data_id"] = "hash_samples";
		v["data"] = (Json::UInt64) m_farm->currentHash((uint64_t)_miner);
		v["type"] = "notify";
		if (SteadyClock::now() > m_hashSampleStop)
		{
			m_hashSamplesTimer->stop();
			v["last_sample"] = true;
		}
		udp->send_packet(v);

	}

	/*-----------------------------------------------------------------------------------
	* assembleGPUTemps
	*----------------------------------------------------------------------------------*/
	bool assembleGPUTemps(Json::Value& _packet, double _delta)
	{
		Json::Value temps(Json::nullValue);
		std::vector<double> data;
		static std::vector<double> lastReported;

		m_farm->getMinerTemps(data);

		bool res = false;
		if (lastReported.size() == 0)
			// first time here. fill lastReported vectors with zeros.
			lastReported.assign(data.size(), 0);

		// assemble the temps into the json structure & check for delta changes.
		for (int i = 0; i < data.size(); i++)
		{
			temps.append(int(data[i] * 100));
			if (abs(data[i] - lastReported[i]) >= _delta)
				res = true;
		}

		if (res == true)
			lastReported = data;

		_packet["data"] = temps;
		return res;
	}

	/*-----------------------------------------------------------------------------------
	* gpuTempsCB
	*----------------------------------------------------------------------------------*/
	void gpuTempsCB(void* _data)
	{
		LogF << "Trace: MVisRPC.gpuTempsCB";
		double delta = uint64_t(_data) / 100.0;
		Json::Value packet(Json::objectValue);
		packet["data_id"] = "gpu_temps";
		packet["type"] = "notify";
		if (assembleGPUTemps(packet, delta))
			udp->send_packet(packet);
	}

	/*-----------------------------------------------------------------------------------
	* assembleFanSpeeds
	*----------------------------------------------------------------------------------*/
	bool assembleFanSpeeds(Json::Value& _packet, int _delta)
	{
		Json::Value speeds(Json::nullValue);
		std::vector<int> data;
		static std::vector<int> lastReported;

		m_farm->getFanSpeeds(data);

		bool res = false;
		if (lastReported.size() == 0)
			// first time here. fill lastReported vectors with zeros.
			lastReported.assign(data.size(), 0);

		// assemble the speeds into the json structure & check for delta changes.
		for (int i = 0; i < data.size(); i++)
		{
			speeds.append(data[i]);
			if (abs(data[i] - lastReported[i]) >= _delta)
				res = true;
		}

		if (res == true)
			lastReported = data;

		_packet["data"] = speeds;
		return res;
	}

	/*-----------------------------------------------------------------------------------
	* fanSpeedsCB
	*----------------------------------------------------------------------------------*/
	void fanSpeedsCB(void* _data)
	{
		LogF << "Trace: MVisRPC.fanSpeedsCB";
		int delta = (uint64_t) _data;
		Json::Value packet(Json::objectValue);
		packet["data_id"] = "fan_speeds";
		packet["type"] = "notify";
		if (assembleFanSpeeds(packet, delta))
			udp->send_packet(packet);
	}

	/*-----------------------------------------------------------------------------------
	* assembleHashRates
	*   - return true if any of the hash rates changed by more than _delta.  _delta values
	*     are referenced against individual gpu miners, not the overall farm rate, even if
	*     you only want the farm rate.
	*   - hash rates are returned as kH/s
	*----------------------------------------------------------------------------------*/
	bool assembleHashRates(Json::Value& _packet, double _delta)
	{
		bool deltaExceeded;
		Json::Value data(Json::objectValue);
		Json::Value ar(Json::arrayValue);

		m_farm->hashRates().update();
		m_farm->hashRates().deltaExceeded(_delta, deltaExceeded);

		data["farm_rate"] = int(m_farm->hashRates().farmRate() * 1000);

		for (std::size_t i = 0; i < m_farm->minerCount(); i++)
			ar.append(int(m_farm->hashRates().minerRate(i) * 1000));

		data["miner_rates"] = ar;

		_packet["data"] = data;
		return deltaExceeded;
	}

	/*-----------------------------------------------------------------------------------
	* hashRateCB
	*----------------------------------------------------------------------------------*/
	void hashRateCB(void* _data)
	{
		double delta = uint64_t(_data) / 1000.0;
		Json::Value packet(Json::objectValue);
		packet["data_id"] = "hash_rates";
		packet["type"] = "notify";
		if (assembleHashRates(packet, delta))
			udp->send_packet(packet);
	}

	/*-----------------------------------------------------------------------------------
	* peerCountCB
	*----------------------------------------------------------------------------------*/
	void peerCountCB(void* _data)
	{
		static int peerCountLastReported = 0;

		try
		{
			RecursiveGuard l(x_rpc);
			LogF << "Trace: MVisRPC.peerCountCB";
			int delta = uint64_t(_data);
			Json::Value p(Json::nullValue);
			Json::Value result = m_nodeRpc->CallMethod("net_peerCount", p);
			int pc = jsToInt(result.asString());
			if (abs(pc - peerCountLastReported) >= delta)
			{
				peerCountLastReported = pc;
				Json::Value packet(Json::objectValue);
				packet["data_id"] = "peer_count";
				packet["type"] = "notify";
				packet["data"] = pc;
				udp->send_packet(packet);
			}
		}
		catch (jsonrpc::JsonRpcException& e)
		{
			LogB << "Exception: MVisRPC.peerCountCB - " << e.what();
		}
	}

	/*-----------------------------------------------------------------------------------
	* queryAcctBalance
	*----------------------------------------------------------------------------------*/
	bool queryAcctBalance(Json::Value& _packet, double _delta)
	{
		static double acctBalanceLastReported = 0;

		try
		{
			// get the mining account
			RecursiveGuard l(x_rpc);
			LogF << "Trace: MVisRPC.queryAcctBalance";
			Json::Value p(Json::nullValue);
			Json::Value result = m_nodeRpc->CallMethod("eth_coinbase", p);
			// get the balance for that account
			p = Json::nullValue;
			p.append(result.asString());
			p.append("latest");
			result = m_nodeRpc->CallMethod("eth_getBalance", p);
			boost::multiprecision::cpp_dec_float_50 balance(jsToU256(result.asString()));
			boost::multiprecision::cpp_dec_float_50 toEther("1000000000000000000");
			balance = balance / toEther;
			double acctBalance = balance.template convert_to<double>();
			_packet["data"] = acctBalance;
			if (abs(acctBalance - acctBalanceLastReported) >= _delta)
			{
				acctBalanceLastReported = acctBalance;
				return true;
			}
			else
			{
				return false;
			}
		}
		catch (jsonrpc::JsonRpcException& e)
		{
			LogB << "Exception: MVisRPC.queryAcctBalance - " << e.what();
			return false;
		}
	}

	/*-----------------------------------------------------------------------------------
	* acctBalanceCB
	*----------------------------------------------------------------------------------*/
	void acctBalanceCB(void* _data)
	{
		double delta = uint64_t(_data) / 10000.0;
		Json::Value packet(Json::objectValue);
		packet["data_id"] = "account_balance";
		packet["type"] = "notify";
		if (queryAcctBalance(packet, delta))
			udp->send_packet(packet);
	}

	/*-----------------------------------------------------------------------------------
	* onKeepAliveExpired
	*----------------------------------------------------------------------------------*/
	void onKeepAliveExpired()
	{
		disconnect("notify");
	}


private:

	dev::eth::GenericFarm<dev::eth::EthashProofOfWork>* m_farm;
	UDPSocket* udp;

	jsonrpc::HttpClient* m_client;
	::FarmClient* m_nodeRpc;	// this is for talking to the node using the HTTP JSON RPC protocol.
	RecursiveMutex x_rpc;

	TimerCallback* m_hashRateTimer;
	TimerCallback* m_hashSamplesTimer;
	TimerCallback* m_gpuTempsTimer;
	TimerCallback* m_fanSpeedsTimer;
	TimerCallback* m_peerCountTimer;
	TimerCallback* m_acctBalanceTimer;

	bool m_reportBestHash = false;
	bool m_reportWorkPackage = false;
	SteadyClock::time_point m_hashSampleStop;


	TimerCallback* m_keepAlive;

};


}//}

