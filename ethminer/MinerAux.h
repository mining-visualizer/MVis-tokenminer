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

/** @file MinerAux.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 * CLI module for mining.
 */

#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <random>
#include <queue>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim_all.hpp>
#include <boost/optional.hpp>

#include <libdevcore/FileSystem.h>
#include <libdevcore/StructuredLogger.h>
#include <libethcore/Exceptions.h>
#include <libdevcore/SHA3.h>
#include <libdevcore/CommonJS.h>
#include <libethcore/EthashAux.h>
#include <libethcore/EthashCUDAMiner.h>
#include <libethcore/EthashGPUMiner.h>
#include <libethcore/EthashCPUMiner.h>
#include <libethcore/Farm.h>

#include <libethash-cl/ethash_cl_miner.h>

#if ETH_ETHASHCUDA
#include <libethash-cuda/ethash_cuda_miner.h>
#endif

#include <jsonrpccpp/server/connectors/httpserver.h>
#include <jsonrpccpp/client/connectors/httpclient.h>

#include "BuildInfo.h"

#include "PhoneHome.h"
#include "FarmClient.h"

#include <libstratum/EthStratumClient.h>

#include "ProgOpt.h"
#include "Misc.h"
#include "Common.h"
#include "MultiLog.h"

using namespace std;
using namespace dev;
using namespace dev::eth;
using namespace boost::algorithm;

#undef RETURN


/*-----------------------------------------------------------------------------------
* class MinerCLI
*----------------------------------------------------------------------------------*/
class MinerCLI
{
public:

	typedef struct
	{
		string url;
		string rpcPort;
		string stratumPort;
		string stratumPwd;
	} node_t;


	/*-----------------------------------------------------------------------------------
	* constructor
	*----------------------------------------------------------------------------------*/
	MinerCLI()
	{}


	/*-----------------------------------------------------------------------------------
	* loadIniSettings
	*----------------------------------------------------------------------------------*/
	void loadIniSettings()
	{
		// some settings can be specified both on the CLI and in the INI file.  for those that
		// can, first load up the values from the INI file, and then any values on the CLI will
		// overwrite these.

		m_nodes.clear();

		node_t node;
		// main node
		node.url = ProgOpt::Get("Node", "Host");
		node.rpcPort = ProgOpt::Get("Node", "RPCPort");
		node.stratumPort = ProgOpt::Get("Node", "StratumPort");
		node.stratumPwd = ProgOpt::Get("Node", "StratumPwd");
		m_nodes.push_back(node);

		// failover node
		node.url = ProgOpt::Get("Node2", "Host");
		node.rpcPort = ProgOpt::Get("Node2", "RPCPort");
		node.stratumPort = ProgOpt::Get("Node2", "StratumPort");
		node.stratumPwd = ProgOpt::Get("Node2", "StratumPwd");
		m_nodes.push_back(node);
	}

	/*-----------------------------------------------------------------------------------
	* parseNode
	*----------------------------------------------------------------------------------*/
	bool parseNode(char* _arg, char* _argN, string &_url, string &_port)
	{
		string s = string(_argN);
		LowerCase(s);
		// boost asio doesn't like the http:// prefix (stratum mining)
		//if (s.find("http") != string::npos)
		//{
		//	LogS << "Invalid " << _arg << " argument. Do not specify the 'http://' prefix, just IP or Host.";
		//	return false;
		//}

		size_t p = s.find_last_of(":");
		if (p > 0)
		{
			_url = s.substr(0, p);
			if (p + 1 <= s.length())
				_port = s.substr(p + 1);
		}
		else
			_url = s;

		return true;
	}

	/*-----------------------------------------------------------------------------------
	* failOverAvailable
	*----------------------------------------------------------------------------------*/
	bool failOverAvailable()
	{
		return m_nodes[1].url != "";
	}

	/*-----------------------------------------------------------------------------------
	* interpretOption
	*----------------------------------------------------------------------------------*/
	bool interpretOption(int& i, int argc, char** argv)
	{
		string arg = argv[i];
		if ((arg == "-N" || arg == "--node") && i + 1 < argc)
		{
			if (!parseNode(argv[i], argv[++i], m_nodes[0].url, m_nodes[0].rpcPort))
				exit(-1);
		}
		else if ((arg == "-N2" || arg == "--node2") && i + 1 < argc)
		{
			if (!parseNode(argv[i], argv[++i], m_nodes[1].url, m_nodes[1].rpcPort))
				exit(-1);
		}
		else if ((arg == "-I" || arg == "--polling-interval") && i + 1 < argc)
			try {
				m_pollingInterval = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		else if ((arg == "-R" || arg == "--farm-retries") && i + 1 < argc)
			try {
				m_maxFarmRetries = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}

		else if ((arg == "--work-timeout") && i + 1 < argc)
		{
			m_worktimeout = atoi(argv[++i]);
		}
		
		else if (arg == "--opencl-platform" && i + 1 < argc)
			try {
				m_openclPlatform = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		else if (arg == "--opencl-devices" || arg == "--opencl-device")
			while (m_openclDeviceCount < 16 && i + 1 < argc)
			{
				try
				{
					m_openclDevices[m_openclDeviceCount] = stol(argv[++i]);
					++m_openclDeviceCount;
                }
				catch (...)
				{
					i--;
					break;
				}
			}
		else if ((arg == "--cl-work-multiplier" || arg == "--cuda-grid-size")  && i + 1 < argc)
			try {
				m_workSizeMultiplier = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		else if ((arg == "--cl-local-work" || arg == "--cuda-block-size") && i + 1 < argc)
			try {
				m_localWorkSize = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		else if (arg == "--list-devices")
			m_shouldListDevices = true;
		else if (arg == "--export-dag" && argc > i + 1)
		{
			m_exportDAG = true;
			try
			{
				m_exportDAG_blockNum = atoi(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(2);
			}
		}
		else if ((arg == "--cl-extragpu-mem" || arg == "--cuda-extragpu-mem") && i + 1 < argc)
			m_extraGPUMemory = 1000000 * stol(argv[++i]);
		else if (arg == "--allow-opencl-cpu")
			m_clAllowCPU = true;
#if ETH_ETHASHCUDA
		else if (arg == "--cuda-devices")
		{
			while (m_cudaDeviceCount < 16 && i + 1 < argc)
			{
				try
				{
					m_cudaDevices[m_cudaDeviceCount] = stol(argv[++i]);
					++m_cudaDeviceCount;
				}
				catch (...)
				{
					i--;
					break;
				}
			}
		}
		else if (arg == "--cuda-schedule" && i + 1 < argc)
		{
			string mode = argv[++i];
			if (mode == "auto") m_cudaSchedule = 0;
			else if (mode == "spin") m_cudaSchedule = 1;
			else if (mode == "yield") m_cudaSchedule = 2;
			else if (mode == "sync") m_cudaSchedule = 4;
			else
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		}
		else if (arg == "--cuda-streams" && i + 1 < argc)
			m_numStreams = stol(argv[++i]);
#endif
		else if ((arg == "-L" || arg == "--dag-load-mode") && i + 1 < argc)
		{
			string mode = argv[++i];
			if (mode == "parallel") m_dagLoadMode = DAG_LOAD_MODE_PARALLEL;
			else if (mode == "sequential") m_dagLoadMode = DAG_LOAD_MODE_SEQUENTIAL;
			else if (mode == "single")
			{
				m_dagLoadMode = DAG_LOAD_MODE_SINGLE;
				m_dagCreateDevice = stol(argv[++i]);
			}
			else
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		}
		else if (arg == "--benchmark-warmup" && i + 1 < argc)
			try {
				m_benchmarkWarmup = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		else if (arg == "--benchmark-trial" && i + 1 < argc)
			try {
				m_benchmarkTrial = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		else if (arg == "--benchmark-trials" && i + 1 < argc)
			try
			{
				m_benchmarkTrials = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		else if (arg == "-C" || arg == "--cpu")
			m_minerType = MinerType::CPU;
		else if (arg == "-G" || arg == "--opencl")
			m_minerType = MinerType::CL;
		else if (arg == "-P" || arg == "--opencl")
			m_opMode = OperationMode::Pool;
		else if (arg == "-S" || arg == "--opencl")
			m_opMode = OperationMode::Solo;
		else if (arg == "-U" || arg == "--cuda")
		{
			m_minerType = MinerType::CUDA;
		}
		else if (arg == "-X" || arg == "--cuda-opencl")
		{
			m_minerType = MinerType::Mixed;
		}
		else if (arg == "-M" || arg == "--benchmark")
		{
			m_doBenchmark = true;
			if (i + 1 < argc)
			{
				string m = boost::to_lower_copy(string(argv[++i]));
				try
				{
					m_benchmarkBlock = stol(m);
				}
				catch (...)
				{
					if (argv[i][0] == 45) { // check next arg
						i--;
					}
					else {
						LogS << "Invalid " << arg << " option: " << argv[i];
						exit(-1);
					}
				}
			}
		}

		else if ((arg == "-t" || arg == "--mining-threads") && i + 1 < argc)
		{
			try
			{
				m_miningThreads = stol(argv[++i]);
			}
			catch (...)
			{
				LogS << "Invalid " << arg << " option: " << argv[i];
				exit(-1);
			}
		}

		else if (arg == "--config")
		{
			LogS << "Invalid --config option: This must be the first option on the command line.";
			exit(-1);
		}


		else
			return false;

		return true;
	}	// interpretOption


	/*-----------------------------------------------------------------------------------
	* execute
	*----------------------------------------------------------------------------------*/
	void execute()
	{

		if (m_minerType == MinerType::Undefined)
		{
			LogS << "No miner type specfied.  Please include either -C (CPU mining) or -G (OpenCL mining) on the command line";
			exit(-1);
		}

		// list devices
		if (m_shouldListDevices)
		{
			if (m_minerType == MinerType::CL || m_minerType == MinerType::Mixed)
				EthashGPUMiner::listDevices();
#if ETH_ETHASHCUDA
			if (m_minerType == MinerType::CUDA || m_minerType == MinerType::Mixed)
				EthashCUDAMiner::listDevices();
#endif
			if (m_minerType == MinerType::CPU)
				LogS << "--list-devices should be combined with GPU mining flag (-G for OpenCL or -U for CUDA)";
			exit(0);
		}

		m_userAcct = ProgOpt::Get("0xBitcoin", "MinerAcct");
		LowerCase(m_userAcct);
		u256 mAcctNum;
		try
		{
			mAcctNum = u256(m_userAcct);
		}
		catch (...)
		{
			LogS << "'MinerAcct' contains invalid characters in tokenminer.ini";
			exit(0);
		}
		if (m_userAcct.substr(0, 2) != "0x")
		{
			LogS << "Invalid 'MinerAcct' in tokenminer.ini - Miner account should start with '0x'";
			exit(0);
		}
		if (mAcctNum == 0 || m_userAcct.length() != 42)
		{
			LogS << "Invalid 'MinerAcct' in tokenminer.ini";
			exit(0);
		}

		string mps = ProgOpt::Get("0xBitcoin", "MinutesPerShare","Pool");
		LowerCase(mps);
		if (mps == "pool")
			m_minutesPerShare = -1;
		else
		{
			m_minutesPerShare = atoi(mps.c_str());
			if (m_minutesPerShare < 1)
			{
				LogS << "Invalid 'MinutesPerShare' in tokenminer.ini. Defaulting to Pool";
				m_minutesPerShare = -1;
			}
		}
		// this is intended to force a specific difficulty level. useful during development & testing, not recommended for the user.
		string diff = ProgOpt::Get("0xBitcoin", "_Difficulty_", "-1");
		m_difficulty = strToInt(diff, -1);

		LogD << " ";
		LogD << "--- Program Start ---";

		if (m_opMode == OperationMode::None)
		{
			LogS << "Operation mode not specfied.  Please include either -S (solo mining) or -P (pool mining) on the command line";
			exit(-1);
		}


		// configure GPU
		if (m_minerType == MinerType::CPU)
		{
			EthashCPUMiner::setNumInstances(m_miningThreads);
		}
		else if (m_minerType == MinerType::CL || m_minerType == MinerType::Mixed)
		{
			if (m_openclDeviceCount > 0)
			{
				EthashGPUMiner::setDevices(m_openclDevices, m_openclDeviceCount);
				m_miningThreads = m_openclDeviceCount;
			}
			
			if (!EthashGPUMiner::configureGPU(
					m_localWorkSize,
					m_workSizeMultiplier,
					m_openclPlatform,
					m_openclDevice,
					m_clAllowCPU,
					m_extraGPUMemory,
					0,
					m_dagLoadMode,
					m_dagCreateDevice
				))
				exit(1);
			EthashGPUMiner::setNumInstances(m_miningThreads);
		}
		else if (m_minerType == MinerType::CUDA || m_minerType == MinerType::Mixed)
		{
#if ETH_ETHASHCUDA
			if (m_cudaDeviceCount > 0)
			{
				EthashCUDAMiner::setDevices(m_cudaDevices, m_cudaDeviceCount);
				m_miningThreads = m_cudaDeviceCount;
			}
			
			EthashCUDAMiner::setNumInstances(m_miningThreads);
			if (!EthashCUDAMiner::configureGPU(
				m_localWorkSize,
				m_workSizeMultiplier,
				m_numStreams,
				m_extraGPUMemory,
				m_cudaSchedule,
				0,
				m_dagLoadMode,
				m_dagCreateDevice
				))
				exit(1);
#else
			cerr << "Selected CUDA mining without having compiled with -DETHASHCUDA=1 or -DBUNDLE=cudaminer" << endl;
			exit(1);
#endif
		}

		if (m_doBenchmark)
			doBenchmark(m_minerType, m_benchmarkWarmup, m_benchmarkTrial, m_benchmarkTrials);
		else
		{
			GenericFarm<EthashProofOfWork> f(m_opMode);
			f.start(createMiners(m_minerType, &f));

			int i = 0;
			while (true)
			{
				if (m_nodes[i].url != "")
				{
					if (m_nodes[i].stratumPort != "")
						doStratum(f, m_nodes[i].url, m_nodes[i].rpcPort, m_nodes[i].stratumPort);
					else
						doFarm(f, m_nodes[i].url, m_nodes[i].rpcPort);
				}
				if (f.shutDown)
					break;
				LogB << "Switching to failover node";
				i = ++i % 2;
			}
		}
			
	}	// execute


	/*-----------------------------------------------------------------------------------
	* streamHelp
	*----------------------------------------------------------------------------------*/
	static void streamHelp(ostream& _out)
	{
		_out
			<< " Node configuration:" << endl
			<< "    -N, --node <host:rpc_port>  Host address and RPC port of your node/mining pool. (default: 127.0.0.1:8545)" << endl
			<< "    -N2, --node2 <host:rpc_port>  Failover node/mining pool (default: disabled)" << endl
			<< "    -I, --polling-interval <n>  Check for new work every <n> milliseconds (default: 2000). " << endl
			<< "    -R, --farm-retries <n> Number of retries until switch to failover (default: 4)" << endl
			<< endl
			<< " Benchmarking mode:" << endl
			<< "    -M, --benchmark  Benchmark for mining and exit" << endl
			<< "    --benchmark-warmup <seconds>  Set the duration of warmup for the benchmark tests (default: 8)." << endl
			<< "    --benchmark-trial <seconds>  Set the duration for each trial for the benchmark tests (default: 3)." << endl
			<< "    --benchmark-trials <n>  Set the number of benchmark tests (default: 5)." << endl
			<< endl
			<< " Mining configuration:" << endl
			<< "    -P  Pool mining" << endl
			<< "    -S  Solo mining" << endl
			<< "    -C,--cpu  CPU mining" << endl
			<< "    -G,--opencl  When mining use the GPU via OpenCL." << endl
			<< "    --cl-local-work <n> Set the OpenCL local work size. Default is " << toString(ethash_cl_miner::c_defaultLocalWorkSize) << endl
			<< "    --cl-work-multiplier <n> This value multiplied by the cl-local-work value equals the number of hashes computed per kernel " << endl
			<< "       run (ie. global work size). (Default: " << toString(ethash_cl_miner::c_defaultWorkSizeMultiplier) << ")" << endl
			<< "    --opencl-platform <n>  When mining using -G/--opencl use OpenCL platform n (default: 0)." << endl
			<< "    --opencl-device <n>  When mining using -G/--opencl use OpenCL device n (default: 0)." << endl
			<< "    --opencl-devices <0 1 ..n> Select which OpenCL devices to mine on. Default is to use all" << endl
			<< "    -t, --mining-threads <n> Limit number of CPU/GPU miners to n (default: use everything available on selected platform)" << endl
			<< "    --allow-opencl-cpu  Allows CPU to be considered as an OpenCL device if the OpenCL platform supports it." << endl
			<< "    --list-devices List the detected OpenCL/CUDA devices and exit. Should be combined with -G or -U flag" << endl
			<< "    --cl-extragpu-mem <n> Set the memory (in MB) you believe your GPU requires for stuff other than mining. default: 0" << endl
			<< endl
			<< " Miscellaneous Options:" << endl
			<< "    --config <FileSpec>  - Full path to an INI file containing program options. Default location is 1) the executable folder, or " << endl
			<< "                           if not there, then in 2) %LocalAppData%/tokenminer/tokenminer.ini (Windows) or " << endl
			<< "                           $HOME/.config/tokenminer/tokenminer.ini (Linux).  If this option is specified,  it must appear " << endl
			<< "                           before all others. " << endl
			;
	}	// streamHelp



	/*-----------------------------------------------------------------------------------
	* createMiners
	*----------------------------------------------------------------------------------*/
	GenericFarm<EthashProofOfWork>::miners_t createMiners(MinerType _minerType, GenericFarm<EthashProofOfWork>* _f)
	{
		unsigned instances;
		GenericFarm<EthashProofOfWork>::CreateInstanceFn create;
		GenericFarm<EthashProofOfWork>::miners_t miners;

		if (_minerType == MinerType::CPU)
		{
			instances = EthashCPUMiner::instances();
			create = [] (GenericFarm<EthashProofOfWork>* _farm, unsigned _index) { return new EthashCPUMiner(_farm, _index); };
		}
		else if (_minerType == MinerType::CL)
		{
			instances = EthashGPUMiner::instances();
			create = [] (GenericFarm<EthashProofOfWork>* _farm, unsigned _index) { return new EthashGPUMiner(_farm, _index); };
		}
		else if (_minerType == MinerType::CUDA)
		{
#if ETH_ETHASHCUDA
			instances = EthashCUDAMiner::instances();
			create = [] (GenericFarm* _farm, unsigned _index) { return new EthashCUDAMiner(_farm, _index); };
#endif
		}

		for (unsigned i = 0; i < instances; ++i)
			miners.push_back(create(_f, i));

		return miners;
	}

	void shutdown()
	{
		m_shutdown = true;
	}


private:

	/*-----------------------------------------------------------------------------------
	* doBenchmark
	*----------------------------------------------------------------------------------*/
	void doBenchmark(MinerType _m, unsigned _warmupDuration = 8, unsigned _trialDuration = 3, unsigned _trials = 5)
	{
		Ethash::BlockHeader genesis;
		genesis.setNumber(m_benchmarkBlock);
		genesis.setDifficulty(1 << 18);

		GenericFarm<EthashProofOfWork> f(m_opMode);

		string platformInfo = _m == MinerType::CPU ? "CPU" : (_m == MinerType::CL ? "CL" : "CUDA");
		LogS << "Benchmarking on platform: " << platformInfo;

		h256 target = h256(1);	
		bytes challenge(32);
		f.start(createMiners(_m, &f));
		f.setWork(challenge, target);

		vector<double> results;
		double mean = 0;
		double innerMean = 0;

		while (!f.isMining())
			this_thread::sleep_for(chrono::milliseconds(1000));

		LogS << "Warming up...";
		for (unsigned i = 0; i <= _warmupDuration; i++)
		{
			f.hashRates().update();
			this_thread::sleep_for(chrono::seconds(1));
		}
		for (unsigned i = 0; i < _trials; ++i)
		{
			cout << "Trial " << i+1 << "... ";
			for (unsigned s = 0; s < _trialDuration; s++)
			{
				this_thread::sleep_for(chrono::milliseconds(1000));
				f.hashRates().update();
			}
			cout << f.hashRates().farmRate() << endl;
			results.push_back(f.hashRates().farmRate());
			mean += results.back();
		}
		f.stop();
		sort(results.begin(), results.end());
		int j = -1;
		for (auto const& r : results)
			if (++j > 0 && j < (int) _trials - 1)
				innerMean += r;
		innerMean /= (_trials - 2);
		cout << "min/mean/max: " << results.front() << "/" << (mean / _trials) << "/" << results.back() << " MH/s" << endl;
		cout << "inner mean: " << innerMean << " MH/s" << endl;

		exit(0);
	}	// doBenchmark


	/*-----------------------------------------------------------------------------------
	* elapsedSeconds
	*----------------------------------------------------------------------------------*/
	std::string elapsedSeconds(Timer _time)
	{
		unsigned seconds = _time.elapsedSeconds();
		std::string s = "0" + std::to_string(seconds % 60);
		return std::to_string(seconds / 60) + ":" + s.substr(s.length() - 2);
	}	// elapsedSeconds
	
	/*-----------------------------------------------------------------------------------
	* positionedOutput
	*----------------------------------------------------------------------------------*/
	void positionedOutput(OperationMode _opMode, GenericFarm<EthashProofOfWork> &f, Timer lastBlockTime, uint64_t tokenBalance, 
						  uint64_t _difficulty, h256 _target)
	{
		int y = 2;
		f.hashRates().update();
		if (f.minerCount() <= 4)
		{
			LogXY(1, 1) << "Rates:" << f.hashRates() << " | Temp: " << f.getMinerTemps() << " | Fan: " << f.getFanSpeeds() << "         ";
		} 
		else
		{
			LogXY(1, 1) << "Rates:" << f.hashRates()  << "         ";
			LogXY(1, 2) << "Temp: " << f.getMinerTemps() << " | Fan: " << f.getFanSpeeds() << "         ";
			y = 3;
		}
		if (_opMode == OperationMode::Solo)
		{
			LogXY(1, y) << "Block #: " << f.currentBlock << " | Block time: " << elapsedSeconds(lastBlockTime)
						<< " | Solutions: " << f.getSolutionStats().getAccepts() << " | Tokens: " << tokenBalance << "      ";
		} 
		else
		{
			LogXY(1, y) << "Difficulty: " << _difficulty << " | Shares: " << f.getSolutionStats().getAccepts() << " | Tokens: " << tokenBalance << "      ";
		}
	}

	h256 targetFromDiff(uint64_t _difficulty)
	{
		// 2^234
		u256 pow_2_234 = u256("0x040000000000000000000000000000000000000000000000000000000000");
		return pow_2_234 / _difficulty;
	}

	uint64_t diffFromTarget(h256 _target)
	{
		// 2^234
		u256 pow_2_234 = u256("0x040000000000000000000000000000000000000000000000000000000000");
		u256 t = pow_2_234 / u256(_target);
		return static_cast<uint64_t>(t);
	}

	/*-----------------------------------------------------------------------------------
	* calcFinalTarget
	*----------------------------------------------------------------------------------*/
	void calcFinalTarget(GenericFarm<EthashProofOfWork>& f, h256& _target, uint64_t& _difficulty)
	{
		// on input we're expecting that target and difficulty are set to the values specified by the pool.
		static float s_lastRate = 0;

		if (m_difficulty != -1)
		{
			_difficulty = m_difficulty;
			_target = targetFromDiff(_difficulty);
		}
		else
		{
			// if we're going by the pool difficulty, do nothing
			if (m_minutesPerShare == -1) return;

			float currentRate = f.hashRates().farmRate();
			LogF << "Trace: calcFinalTarget - s_lastRate : " << s_lastRate << ", farmRate : " << currentRate;
			// the first time around we usually don't have good hash rate information yet.
			if (s_lastRate == 0 || currentRate == 0)
				currentRate = f.minerCount() * 400;	// take a guess at hash rates

			// only recalculate if change is > 10%
			if (s_lastRate == 0 || abs(s_lastRate - currentRate) / s_lastRate > 0.1)
			{
				s_lastRate = currentRate;
				LogF << "Trace: calcFinalTarget - New s_lastRate : " << s_lastRate;
			}

			double divisor = s_lastRate * 1000000.0 * m_minutesPerShare * 60;
			uint64_t target64 = (divisor == 0) ? 0 : ~uint64_t(0) / divisor;
			u256 target256 = target64;
			_target = target256 << 192;
			_difficulty = diffFromTarget(_target);
		}

		LogF << "Trace: calcFinalTarget - Target : " << std::hex << std::setw(16) << std::setfill('0') << upper64OfHash(_target)
			<< ", difficulty : " << std::dec << _difficulty;
	}

	/*-----------------------------------------------------------------------------------
	* calcDevFeeTimes
	*----------------------------------------------------------------------------------*/
	void calcDevFeeTimes(int& _nextDevFeeSwitch, int& _userFeeTime, int& _devFeeTime)
	{
		#define FEEBLOCKTIME 4 * 60 * 60		// devFee switching is done in 4 hour blocks

		//string sDevPercent = ProgOpt::Get("General", "DevFee", "2.0");
		string sDevPercent = "1.0";
		if (!isNumeric(sDevPercent))
		{
			LogB << "Invalid DevFee in tokenminer.ini!  Defaulting to 1.0%.";
			sDevPercent = "1.0";
		}
		_devFeeTime = std::stod(sDevPercent) / 100.0 * FEEBLOCKTIME;
		if (_devFeeTime < 0)
		{
			LogB << "Invalid DevFee in tokenminer.ini!  Defaulting to 1.0%.";
			_devFeeTime = 1.0 / 100.0 * FEEBLOCKTIME;
		}
		_userFeeTime = FEEBLOCKTIME - _devFeeTime;
		_nextDevFeeSwitch = _devFeeTime == 0 ? 0 : _userFeeTime;

	}

	/*-----------------------------------------------------------------------------------
	* doFarm
	*----------------------------------------------------------------------------------*/
	void doFarm(GenericFarm<EthashProofOfWork>& f, string _nodeURL, string _rpcPort)
	{
		Timer lastHashRateDisplay;
		Timer lastBlockTime;
		Timer lastBalanceCheck;
		Timer lastGetWork;
		Timer lastCheckTx;
		Timer devFeeSwitch;

		// the absolute value of nextDevFeeSwitch is the time until the next switch.
		// if the value is >= 0, that means we are currently mining to the user's account,
		// if it's negative, we are mining to the dev account.
		int nextDevFeeSwitch;

		// the amount of time to spend mining for the user/dev respectively
		int userFeeTime, devFeeTime;

		calcDevFeeTimes(nextDevFeeSwitch, userFeeTime, devFeeTime);

		unsigned farmRetries = 0;
		int maxRetries = failOverAvailable() ? m_maxFarmRetries : c_StopWorkAt;
		bool connectedToNode = false;
		bool gasPriceBidding = ProgOpt::Get("0xBitcoin", "GasPriceBidding", "0") == "1" && m_opMode == OperationMode::Solo;

		LogS << "Connecting to " << _nodeURL + ":" + _rpcPort << " ...";

		// if solo mining, both workRPC and nodeRPC point to the mainNet node (whatever the user specifies)
		// if pool mining, workRPC points to the mining pool, and nodeRPC points to Infura

		jsonrpc::HttpClient client(_nodeURL + ":" + _rpcPort);
		FarmClient workRPC(client, m_opMode, m_userAcct);

		jsonrpc::HttpClient* nodeClient;
		FarmClient* nodeRPC = &workRPC;
		if (m_opMode == OperationMode::Pool)
		{
			nodeClient = new jsonrpc::HttpClient("https://mainnet.infura.io/J9KBwsJ0q1LMIQvzDlGC:8545");
			nodeRPC = new FarmClient(*nodeClient, OperationMode::Solo, m_userAcct);
		}
		else
			f.hashingAcct = m_userAcct;
		h256 target;
		bytes challenge;
		deque<bytes> recentChallenges;
		uint64_t difficulty = 0;

		int tokenBalance = nodeRPC->tokenBalance();

		while (!m_shutdown)
		{
			try
			{
				h256 solution;
				int solutionMiner;

				while (!f.solutionFound(solution, solutionMiner) && !f.shutDown)
				{
					if (!challenge.empty())
					{
						if (lastHashRateDisplay.elapsedSeconds() >= 2.0 && f.isMining())
						{
							int blkNum = 0;
							try
							{
								if (m_opMode == OperationMode::Solo)
									blkNum = nodeRPC->getBlockNumber() + 1;
							}
							catch (...) {}
							if (blkNum != 0 && blkNum != f.currentBlock)
							{
								f.currentBlock = blkNum;
								lastBlockTime.restart();
							}
							positionedOutput(m_opMode, f, lastBlockTime, tokenBalance, difficulty, target);
							lastHashRateDisplay.restart();
						}
					}
					if (lastBalanceCheck.elapsedSeconds() >= 60)
					{
						tokenBalance = nodeRPC->tokenBalance();
						lastBalanceCheck.restart();
					}

					h256 _target;
					bytes _challenge;
					if (lastGetWork.elapsedMilliseconds() > m_pollingInterval || !connectedToNode)
					{
						if (m_opMode == OperationMode::Pool)
						{
							workRPC.getWorkPool(_challenge, _target, difficulty, f.hashingAcct);
							// if we're choosing our own difficulty instead of using the pools, calcFinalTarget will make the adjustment
							calcFinalTarget(f, _target, difficulty);
						}
						else
							workRPC.getWorkSolo(_challenge, _target);

						lastGetWork.restart();

						if (!connectedToNode)
						{
							connectedToNode = true;
							LogS << "Connection established.";
						}
						farmRetries = 0;

						if (_challenge != challenge)
						{
							// when queried for the most recent challenge, infura nodes will occasionally respond with the 
							// previous one. this only applies to solo mining.  when pool mining, always use what the pool gives us.
							bool seenBefore = false;
							for (bytes c : recentChallenges)
								seenBefore = (seenBefore || (c == _challenge));
							if (!seenBefore || m_opMode == OperationMode::Pool)
							{
								recentChallenges.push_front(_challenge);
								if (recentChallenges.size() > 5)
									recentChallenges.pop_back();
								challenge = _challenge;
								target = _target;
								LogB << "New challenge : " << toHex(_challenge).substr(0, 8);
								f.setWork(challenge, target);
								workRPC.setChallenge(challenge);
							}
						}
						if (_target != target)
						{
							target = _target;
							f.setWork(challenge, target);
						}
					}

					if (lastCheckTx.elapsedMilliseconds() > 1000 && m_opMode == OperationMode::Solo)
					{
						nodeRPC->checkPendingTransactions();
						if (gasPriceBidding)
							nodeRPC->txpoolScanner();
						lastCheckTx.restart();
					}

					if (nextDevFeeSwitch != 0 && devFeeSwitch.elapsedSeconds() > abs(nextDevFeeSwitch))
					{
						if (nextDevFeeSwitch < 0)
						{
							LogB << "Switching to user mining.";
							nextDevFeeSwitch = userFeeTime;
							workRPC.devFeeMining = false;
						} 
						else
						{
							LogB << "Switching to dev fee mining.";
							nextDevFeeSwitch = (-1) * devFeeTime;
							workRPC.devFeeMining = true;
						}
						devFeeSwitch.restart();
					}

					this_thread::sleep_for(chrono::milliseconds(200));
				}

				if (f.shutDown)
					break;

				bytes hash(32);
				h160 sender(f.hashingAcct);
				keccak256_0xBitcoin(challenge, sender, solution, hash);
				if (h256(hash) < target) {
					if (m_opMode == OperationMode::Pool)
					{
						LogS << "Solution found; Submitting to pool" << ((nextDevFeeSwitch >= 0) ? "" : " on the dev account");
						LogD << "Solution found: challenge = " << toHex(challenge).substr(0, 8) << ", nonce = " << solution.hex().substr(0, 8);
						workRPC.submitWorkPool(solution, hash, challenge, difficulty);
					}
					else
					{
						LogB << "Solution found; Submitting to node";
						workRPC.submitWorkSolo(solution, hash, challenge);
					}
					f.recordSolution(SolutionState::Accepted, false, solutionMiner);
				} else {
					LogB << "Solution found, but invalid.  Possibly stale.";
					f.recordSolution(SolutionState::Accepted, true, solutionMiner);
				}
			}
			catch (jsonrpc::JsonRpcException& e)
			{
				connectedToNode = false;
				LogB << "An error occurred communicating with the node : " << e.what();
				LogS << "Trying again in 5 seconds ...";
				farmRetries++;
				if (farmRetries == maxRetries)
				{
					// if there's a failover available, we'll switch to it, but worst case scenario, it could be 
					// unavailable as well, so at some point we should pause mining.  we'll do it here.
					challenge.clear();
					f.setWork(challenge, target);
					LogS << "Mining paused ...";
					if (failOverAvailable())
						break;
				}
				this_thread::sleep_for(chrono::seconds(5));
				LogS << "Connecting to " << _nodeURL + ":" + _rpcPort << " ...";
			}
			catch (const std::exception& e)
			{
				LogB << "Exception: MinerAux::doFarm - " << e.what();
			}
		}

		nodeRPC->closeTxFilter();

	}	// doFarm


	/*-----------------------------------------------------------------------------------
	* doStratum
	*----------------------------------------------------------------------------------*/
	void doStratum(GenericFarm<EthashProofOfWork>& f, string _nodeURL, string _rpcPort, string _stratumPort)
	{
		// retry of zero means retry forever, since there is no failover.
		int maxRetries = failOverAvailable() ? m_maxFarmRetries : 0;
		EthStratumClient client(_nodeURL, _stratumPort, maxRetries, m_worktimeout, m_userAcct);

		Timer lastHashRateDisplay;
		Timer lastBlockTime;
		Timer lastBalanceCheck;

		uint64_t difficulty = 0;
		h256 target;
		bytes challenge;

		h256 solution;
		int solutionMiner = -1;

		jsonrpc::HttpClient rpcClient("https://mainnet.infura.io/J9KBwsJ0q1LMIQvzDlGC:8545");
		FarmClient nodeRPC(rpcClient, OperationMode::Solo, m_userAcct);

		int tokenBalance = nodeRPC.tokenBalance();

		while (client.isRunning())
		{

			while (!f.solutionFound(solution, solutionMiner) && !f.shutDown && client.isRunning())
			{
				if (lastHashRateDisplay.elapsedSeconds() >= 2.0 && client.isConnected() && f.isMining())
				{
					positionedOutput(OperationMode::Pool, f, lastBlockTime, tokenBalance, difficulty, target);
					lastHashRateDisplay.restart();
				}

				h256 _target;
				bytes _challenge;
				client.getWork(_challenge, _target, difficulty, f.hashingAcct);
				// if we're choosing our own difficulty instead of using the pools, calcFinalTarget will make the adjustment
				calcFinalTarget(f, _target, difficulty);

				if (_challenge != challenge)
				{
					challenge = _challenge;
					target = _target;
					LogB << "New challenge : " << toHex(_challenge).substr(0, 8);
					f.setWork(challenge, target);
				}
				if (_target != target)
				{
					target = _target;
					f.setWork(challenge, target);
				}

				if (lastBalanceCheck.elapsedSeconds() >= 60)
				{
					tokenBalance = nodeRPC.tokenBalance();
					lastBalanceCheck.restart();
				}

				this_thread::sleep_for(chrono::milliseconds(200));
			}

			if (f.shutDown)
				break;

			if (solutionMiner != -1)
			{
				bytes hash(32);
				h160 sender(f.hashingAcct);
				keccak256_0xBitcoin(challenge, sender, solution, hash);
				if (h256(hash) < target)
				{
					LogS << "Solution found; Submitting to pool";
					LogD << "Solution found: challenge = " << toHex(challenge).substr(0, 8) << ", nonce = " << solution.hex().substr(0, 8);
					client.submitWork(solution, hash, challenge, difficulty);
					f.recordSolution(SolutionState::Accepted, false, solutionMiner);
				} else
				{
					LogB << "Solution found, but invalid.  Possibly stale.";
					f.recordSolution(SolutionState::Accepted, true, solutionMiner);
				}
				solutionMiner = -1;
			}

		}


	}	// doStratum


private:

	/// Mining options
	MinerType m_minerType = MinerType::Undefined;
	OperationMode m_opMode = OperationMode::None;
	int m_minutesPerShare = 2;	  // set to -1 to use pool difficulty
	int m_difficulty = -1;		  // useful during development & testing
	unsigned m_openclPlatform = 0;
	unsigned m_openclDevice = 0;
	unsigned m_miningThreads = UINT_MAX;
	bool m_shouldListDevices = false;
	bool m_exportDAG = false;
	unsigned m_exportDAG_blockNum;
	bool m_clAllowCPU = false;
#if ETH_ETHASHCL || !ETH_TRUE
	unsigned m_openclDeviceCount = 0;
	unsigned m_openclDevices[16];
#if !ETH_ETHASHCUDA || !ETH_TRUE
	unsigned m_workSizeMultiplier = ethash_cl_miner::c_defaultWorkSizeMultiplier;
	unsigned m_localWorkSize = ethash_cl_miner::c_defaultLocalWorkSize;
#endif
#endif
#if ETH_ETHASHCUDA || !ETH_TRUE
	unsigned m_workSizeMultiplier = ethash_cuda_miner::c_defaultGridSize;
	unsigned m_localWorkSize = ethash_cuda_miner::c_defaultBlockSize;
	unsigned m_cudaDeviceCount = 0;
	unsigned m_cudaDevices[16];
	unsigned m_numStreams = ethash_cuda_miner::c_defaultNumStreams;
	unsigned m_cudaSchedule = 4; // sync
#endif
	// default value was 350MB of GPU memory for other stuff (windows system rendering, e.t.c.)
	unsigned m_extraGPUMemory = 0;// 350000000; don't assume miners run desktops...
	unsigned m_dagLoadMode = 0; // parallel
	unsigned m_dagCreateDevice = 0;
	/// Benchmarking params
	bool m_doBenchmark = false;
	bool m_phoneHome = false;
	unsigned m_benchmarkWarmup = 8;
	unsigned m_benchmarkTrial = 3;
	unsigned m_benchmarkTrials = 5;
	unsigned m_benchmarkBlock = 0;
	
	std::vector<node_t> m_nodes;

	unsigned m_maxFarmRetries = 4;
	unsigned m_pollingInterval = 2000;
	unsigned m_worktimeout = 180;
	bool m_shutdown = false;

	string m_userAcct;

};
