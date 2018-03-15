/*
 This file is part of cpp-ethereum.

 cpp-ethereum is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 cpp-ethereum is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
 */
/** @file Farm.h
 * @author Gav Wood <i@gavwood.com>
 * @date 2015
 */

#pragma once

#include <thread>
#include <list>
#include <atomic>
#include <libdevcore/Common.h>
#include <libdevcore/Worker.h>
#include <libethcore/Common.h>
#include <libethcore/Miner.h>
#include <libethcore/BlockInfo.h>
#include <ethminer/DataLogger.h>
#include <ethminer/MultiLog.h>



namespace dev
{

namespace eth
{
	// pause mining after this number of retries (lost comm with node)
	unsigned const c_StopWorkAt = 4;



/*-----------------------------------------------------------------------------------
* class GenericFarm
*----------------------------------------------------------------------------------*/
template <class PoW>
class GenericFarm
{
public:
	using WorkPackage = typename PoW::WorkPackage;
	using Solution = typename PoW::Solution;
	using Miner = GenericMiner<PoW>;

	using SolutionFound = std::function<bool(Solution const&, int)>;
	using SolutionFoundToken = std::function<bool(h256 const&, int)>;
	using BestHashFn = boost::function<void(uint64_t const)>;
	using SetWorkFn = boost::function<void(uint64_t const)>;
	using SolutionProcessedFn = boost::function<bool(unsigned, SolutionState, bool, int)>;
	using CloseHitFn = boost::function<bool(uint64_t const, unsigned const, int const)>;
	using HashFaultFn = boost::function<bool(int const)>;

	using CountInstancesFn = std::function<unsigned()>;
	using CreateInstanceFn = std::function<GenericMiner<PoW>*(GenericFarm<PoW>* _farm, unsigned _index)>;

	typedef std::vector<GenericMiner<PoW>*> miners_t;


	/*-----------------------------------------------------------------------------------
	* class HashRates
	*----------------------------------------------------------------------------------*/
	class HashRates
	{
	public:

		// this maintains hashrates per miner. all rates are MH/s. 
		// you need to call update before retrieving farmRate or minerRates.

		HashRates(GenericFarm<PoW>* _f) : m_farm(_f) {}

		void init()
		{
			for (std::size_t i = 0; i < m_farm->minerCount(); i++)
				m_minerRates.push_back(EMA(3));
		}

		/**
		* @brief gather and compute latest hash rates. 
		*/
		void update()
		{
			WriteGuard l(x_hashRates);
			m_farmRate = 0;
			std::string rates;
			for (std::size_t i = 0; i < m_farm->minerCount(); i++)
			{
				m_minerRates[i].newVal(m_farm->m_miners[i]->getHashRate());
				rates.append(toString(m_minerRates[i].value()) + ", ");
				m_farmRate += m_minerRates[i].value();
			}
			rates.resize(rates.size() - 2);
			LogF << "HashRates.update: " << m_farmRate << " [" << rates << "]";
		}

		friend std::ostream& operator<< (std::ostream &out, const HashRates &rates)
		{
			std::string sep;
			char szBuff[20];

			ReadGuard l(rates.x_hashRates);
			sprintf(szBuff, " %.1f", rates.m_farmRate);
			out << szBuff << " MH/s";
			if (rates.m_minerRates.size() > 1)
			{
				out << " [";
				for (std::size_t i = 0; i < rates.m_farm->minerCount(); i++)
				{
					sprintf(szBuff, "%.1f", rates.minerRate(i));
					out << sep << szBuff;
					sep = ", ";
				}
				out << "]";
			}
			if (rates.m_farm->anyThrottling())
				out << " - THROTTLING!! ";
			return out;
		}

		float farmRate() 
		{ 
			ReadGuard l(x_hashRates); 
			return m_farmRate; 
		}

		float minerRate(int _miner) const
		{ 
			ReadGuard l(x_hashRates); 
			return m_minerRates[_miner].value();
		}

		// return true if any of the miner hash rates have changed by more than _delta.  
		// you need to call update() first before calling this.
		// currently there is no support for obtaining delta change info on the farm rate.
		void deltaExceeded(float _delta, bool& _deltaExceeded)
		{
			_deltaExceeded = false;

			if (m_lastReportedRate.size() == 0)
			{
				// first time here. fill lastReported vectors with zeros.
				m_lastReportedRate.assign(m_farm->minerCount(), 0);
			}

			for (int i = 0; i < m_minerRates.size(); i++)
			{
				if (abs(m_minerRates[i].value() - m_lastReportedRate[i]) >= _delta)
				{
					_deltaExceeded = true;
					m_lastReportedRate[i] = m_minerRates[i].value();
				}
			}
		}

		GenericFarm<PoW>* m_farm = nullptr;

	private:
		mutable SharedMutex x_hashRates;
		std::vector<EMA> m_minerRates;
		std::vector<float> m_lastReportedRate;	// used for delta change calculations
		float m_farmRate;

	};	// class HashRates

//////////////////////////////////////////////////////////////////////////////////////////////

	/*-----------------------------------------------------------------------------------
	* GenericFarm Constructor
	*----------------------------------------------------------------------------------*/
	GenericFarm() : m_onBestHash(NULL)
	{
		m_hashRates = new HashRates(this);
		if (ProgOpt::Get("CloseHits", "Enabled") == "1")
		{
			closeHitThreshold = std::stoull(ProgOpt::Get("CloseHits", "CloseHitThreshold").c_str());
			workUnitFreq = std::stoull(ProgOpt::Get("CloseHits", "WorkUnitFrequency").c_str());
		}
	}

	~GenericFarm()
	{
		stop();
	}


	/*-----------------------------------------------------------------------------------
	* setWork_token
	*----------------------------------------------------------------------------------*/
	void setWork_token(bytes _challenge, h256 _target)
	{
		LogF << "Trace: GenericFarm::setWork, challenge=" << toHex(_challenge).substr(0, 8);
		if (m_onSetWork)
			m_onSetWork(upper64OfHash(_target));

		WriteGuard l(x_minerWork);
		if (_challenge == challenge)
			return;
		challenge = _challenge;
		target = _target;
		for (auto const& m: m_miners)
			m->setWork_token(challenge, target);
	}


	/*-----------------------------------------------------------------------------------
	* setWork
	*----------------------------------------------------------------------------------*/
	void setWork(WorkPackage const& _wp)
	{
		LogF << "Trace: GenericFarm::setWork";
		if (m_onSetWork)
			m_onSetWork(upper64OfHash(_wp.boundary));

		WriteGuard l(x_minerWork);
		if (_wp.headerHash == m_work.headerHash && _wp.startNonce == m_work.startNonce)
			return;
		m_work = _wp;
		for (auto const& m: m_miners)
			m->setWork(m_work);
    }


	/*-----------------------------------------------------------------------------------
	* start
	*----------------------------------------------------------------------------------*/
	void start(const miners_t& _miners)
	{
		LogF << "Trace: GenericFarm.start";
		WriteGuard l(x_minerWork);
		m_miners = _miners;
		m_hashFaults.assign(m_miners.size(), 0);

		m_bestHash = logger.retrieveBestHash();
		m_hashRates->init();
		// can't call setWork until we've initialized the hash rates
		//for (auto const& m : m_miners)
		//	m->setWork_token(challenge, target);

		LogF << "Trace: GenericFarm.start [exit]";
	}

	/*-----------------------------------------------------------------------------------
	* pauseMining
	*----------------------------------------------------------------------------------*/
	void pauseMining()
	{
		for (auto const& m : m_miners)
			m->pause();
	}


	/*-----------------------------------------------------------------------------------
	* stop
	*----------------------------------------------------------------------------------*/
	void stop()
	{
		LogF << "Trace: GenericFarm.stop";
		WriteGuard l(x_minerWork);
		m_miners.clear();
		m_work.reset();
		m_isMining = false;
	}
	
	/*-----------------------------------------------------------------------------------
	* isMining
	*----------------------------------------------------------------------------------*/
	bool isMining()
	{
		return m_isMining;
	}

	/*-----------------------------------------------------------------------------------
	* setIsMining
	*----------------------------------------------------------------------------------*/
	void setIsMining(bool _isMining)
	{ 
		m_isMining = _isMining; 
	}

	/*-----------------------------------------------------------------------------------
	* boundary
	*----------------------------------------------------------------------------------*/
	h256 boundary()
	{ 
		// Retrieve the current boundary value.
		ReadGuard l(x_minerWork); 
		return m_work.boundary; 
	}

	/*-----------------------------------------------------------------------------------
	* currentHash
	*----------------------------------------------------------------------------------*/
	uint64_t currentHash(int _miner)
	{
		// Retrieve a random hash value from the specified miner.
		return m_miners[_miner]->currentHash();
	}

	/*-----------------------------------------------------------------------------------
	* bestHash
	*----------------------------------------------------------------------------------*/
	uint64_t bestHash()
	{ 
		// Retrieve the current bestHash value over all miners.
		ReadGuard l(x_bestHash); 
		return m_bestHash; 
	}

	/*-----------------------------------------------------------------------------------
	* suggestBestHash
	*----------------------------------------------------------------------------------*/
	void suggestBestHash(uint64_t _bh)
	{
		// One of the miners has found a better hash.  record it if it's the best overall.
		if (_bh < m_bestHash)
		{
			LogF << "Trace: GenericFarm::suggestBestHash : hash improvement = " << _bh;
			WriteGuard l(x_bestHash);
			m_bestHash = _bh;
			logger.recordBestHash(_bh);
			if (m_onBestHash)
				m_onBestHash(_bh);
		}
	}

	/*-----------------------------------------------------------------------------------
	* onBestHash
	*----------------------------------------------------------------------------------*/
	void onBestHash(BestHashFn const& _handler)
	{ 
		// set a handler for best hash event.  typically a call MVisRPC, which will send 
		// notification to MVis.
		m_onBestHash = _handler; 
	}

	/*-----------------------------------------------------------------------------------
	* reportCloseHit
	*----------------------------------------------------------------------------------*/
	void reportCloseHit(uint64_t _closeHit, unsigned _work, int _miner)
	{
		// Miner is letting us know it found a close hit. work is the number of seconds elapsed
		// since the previous close hit.

		LogF << "Trace: GenericFarm::reportCloseHit : closeHit = " << _closeHit << ", miner = " << _miner;
		m_closeHits++;
		m_lastCloseHit = _closeHit;
		// if we're connected to MVis, inform it of the close hit, otherwise log to disk
		if (!m_onCloseHit || !m_onCloseHit(_closeHit, _work, _miner))
			logger.recordCloseHit(_closeHit, _work, _miner);
	}

	/*-----------------------------------------------------------------------------------
	* getCloseHits
	*----------------------------------------------------------------------------------*/
	std::string getCloseHits()
	{
		// formatted for screen output
		std::stringstream s;
		s << "Work units: " << m_closeHits << ", Last: ";
		if (m_closeHits == 0)
			s << "...";
		else
			s << m_lastCloseHit;
		return s.str();
	}

	/*-----------------------------------------------------------------------------------
	* onCloseHit
	*----------------------------------------------------------------------------------*/
	void onCloseHit(CloseHitFn const& _handler)
	{
		// set a handler for the close hit event.  typically a call to MVisRPC, which will
		// send notification to MVis.
		m_onCloseHit = _handler;
	}

	/*-----------------------------------------------------------------------------------
	* setCloseHitThresholds
	*----------------------------------------------------------------------------------*/
	void setCloseHitThresholds(uint64_t _closeHitThreshold, uint64_t _workUnitFreq)
	{
		// _closeHitThreshold is expressed as a literal hash value.
		// _workUnitFreq is expressed as the desired number of seconds between close hits. each
		// miner is responsible to convert this to a hash value based on their current hash rate.

		closeHitThreshold = _closeHitThreshold;
		workUnitFreq = _workUnitFreq;

		for (auto const& m : m_miners)
			m->calcWorkUnitThreshold();

		// write the values to the ini file as well.
		ProgOpt::beginUpdating();
		ProgOpt::Put("CloseHits", "CloseHitThreshold", _closeHitThreshold);
		ProgOpt::Put("CloseHits", "WorkUnitFrequency", _workUnitFreq);
		ProgOpt::endUpdating();

	}

	/*-----------------------------------------------------------------------------------
	* reportHashFault
	*----------------------------------------------------------------------------------*/
	void reportHashFault(int _miner)
	{
		// Miner is letting us know it experienced a hash fault.
		LogF << "Trace: GenericFarm::reportHashFault : miner = " << _miner;
		// keep track of numbers for this session
		m_hashFaults[_miner]++;
		// if we're connected to MVis, inform it of the hash fault, otherwise log to disk
		if (!m_onHashFault || !m_onHashFault(_miner))
			logger.recordHashFault(_miner);
	}

	/*-----------------------------------------------------------------------------------
	* onHashFault
	*----------------------------------------------------------------------------------*/
	void onHashFault(HashFaultFn const& _handler)
	{
		// set a handler for the hash fault event.  typically a call to MVisRPC, which will
		// send notification to MVis.
		m_onHashFault = _handler;
	}

	/*-----------------------------------------------------------------------------------
	* getHashFaults
	*----------------------------------------------------------------------------------*/
	std::string getHashFaults()
	{
		// formatted for screen output
		std::string sep;
		std::stringstream s;
		for (auto const& fault : m_hashFaults)
		{
			s << sep << fault;
			sep = ", ";
		}
		return s.str();
	}

	/*-----------------------------------------------------------------------------------
	* onSolutionProcessed
	*----------------------------------------------------------------------------------*/
	void onSolutionProcessed(SolutionProcessedFn const& _handler)
	{
		// set a handler for the solution processed event. typically a call to MVisRPC so
		// it can notify MVis.  this event occurs after we have sent the potential
		// solution to the node for acceptance, and have received a response back.
		m_onSolutionProcessed = _handler;
	}

	/*-----------------------------------------------------------------------------------
	* onSolutionFound
	*----------------------------------------------------------------------------------*/
	void onSolutionFound(SolutionFound const& _handler)
	{
		// set a handler for the solution found event.  typically a lambda expression in
		// the main loop.  this event signifies that a miner has found a solution, but it
		// has not been confirmed by the node / mining pool.
		m_onSolutionFound = _handler;
	}


	/*-----------------------------------------------------------------------------------
	* onSolutionFoundToken
	*----------------------------------------------------------------------------------*/
	void onSolutionFoundToken(SolutionFoundToken const& _handler) {
		// set a handler for the solution found event.  typically a lambda expression in
		// the main loop.  this event signifies that a miner has found a solution, but it
		// has not been confirmed by the node / mining pool.
		m_onSolutionFoundToken = _handler;
	}


	/*-----------------------------------------------------------------------------------
	* solutionFound
	*----------------------------------------------------------------------------------*/
	void solutionFound(SolutionState _state, bool _stale, int _miner)
	{
		// we're being notified (typically by the main loop) as to the acceptance
		// state of a recent solution.

		if (_state == SolutionState::Accepted)
		{
			//LogB << ":) Submitted and accepted.";
			if (_stale)
				m_solutionStats.acceptedStale();
			else
				m_solutionStats.accepted();
		}
		else if (_state == SolutionState::Rejected)
		{
			//LogB << ":-( Not accepted.";
			if (_stale)
				m_solutionStats.rejectedStale();
			else
				m_solutionStats.rejected();
		}
		else
		{
			//LogB << "FAILURE: GPU gave incorrect result!";
			m_solutionStats.failed();
		}

		// if we're connected to MVis, inform them of the solution, otherwise log it to disk
		if (!m_onSolutionProcessed || !m_onSolutionProcessed(currentBlock, _state, _stale, _miner))
		{
			logger.recordSolution(currentBlock, (int) _state, _stale, _miner);
		}

		resetBestHash();

	}	// solutionFound


	/*-----------------------------------------------------------------------------------
	* onSetWork
	*----------------------------------------------------------------------------------*/
	void onSetWork(SetWorkFn const& _handler)
	{ 
		m_onSetWork = _handler; 
	}

	/*-----------------------------------------------------------------------------------
	* resetBestHash
	*----------------------------------------------------------------------------------*/
	void resetBestHash()
	{
		LogF << "Trace: GenericFarm.resetBestHash";
		for (auto const& m : m_miners)
			m->resetBestHash();
		WriteGuard l(x_bestHash);
		m_bestHash = ~uint64_t(0);
	}

	/*-----------------------------------------------------------------------------------
	* getSolutionStats
	*----------------------------------------------------------------------------------*/
	SolutionStats getSolutionStats() {
		return m_solutionStats;
	}
	

	/*-----------------------------------------------------------------------------------
	* work
	*----------------------------------------------------------------------------------*/
	WorkPackage work() const
	{ 
		ReadGuard l(x_minerWork); 
		return m_work; 
	}

	/*-----------------------------------------------------------------------------------
	* minerCount
	*----------------------------------------------------------------------------------*/
	int minerCount()
	{ 
		return m_miners.size(); 
	}

	/*-----------------------------------------------------------------------------------
	* hashRates
	*----------------------------------------------------------------------------------*/
	HashRates& hashRates()
	{ 
		return *m_hashRates; 
	}

	/*-----------------------------------------------------------------------------------
	* getMinerTemps (overloaded)
	*----------------------------------------------------------------------------------*/
	void getMinerTemps(std::vector<double>& _temps)
	{
		_temps.clear();
		for (auto const& m : m_miners)
			_temps.push_back(m->gpuTemp());
	}

	/*-----------------------------------------------------------------------------------
	* getMinerTemps (overloaded)
	*----------------------------------------------------------------------------------*/
	std::string getMinerTemps()
	{
		// formatted for screen output
		std::string sep;
		std::stringstream s;
		for (auto const& m : m_miners)
		{
			s << sep << m->gpuTemp();
			sep = ", ";
		}
		return s.str();
	}

	/*-----------------------------------------------------------------------------------
	* getFanSpeeds (overloaded)
	*----------------------------------------------------------------------------------*/
	void getFanSpeeds(std::vector<int>& _speeds)
	{
		_speeds.clear();
		for (auto const& m : m_miners)
			_speeds.push_back(m->fanSpeed());
	}

	/*-----------------------------------------------------------------------------------
	* getFanSpeeds (overloaded)
	*----------------------------------------------------------------------------------*/
	std::string getFanSpeeds()
	{
		// formatted for screen output
		std::string sep;
		std::stringstream s;
		for (auto const& m : m_miners)
		{
			s << sep << m->fanSpeed();
			sep = ", ";
		}
		return s.str();
	}

	/*-----------------------------------------------------------------------------------
	* setGpuThrottle
	*----------------------------------------------------------------------------------*/
	void setGpuThrottle(int _gpu, int _percent)
	{
		m_miners.at(_gpu)->setThrottle(_percent);
	}


	/*-----------------------------------------------------------------------------------
	* anyThrottling
	*----------------------------------------------------------------------------------*/
	bool anyThrottling(void)
	{
		// return true if any of the miners are throttling.
		int t = 0;
		for (auto const& m : m_miners)
			t += m->throttle();
		return (t > 0);
	}

	/*-----------------------------------------------------------------------------------
	* thermalProtection
	*----------------------------------------------------------------------------------*/
	void thermalProtection(int _neverExceed, double _safetyShutdown)
	{
		for (auto const& m : m_miners)
			m->thermalProtection(_neverExceed, _safetyShutdown);
		// persist these settings to disk.
		ProgOpt::beginUpdating();
		ProgOpt::Put("ThermalProtection", "ThrottleTemp", _neverExceed);
		ProgOpt::Put("ThermalProtection", "ShutDown", _safetyShutdown);
		ProgOpt::endUpdating();
	}

	/*-----------------------------------------------------------------------------------
	* tunePIDController
	*----------------------------------------------------------------------------------*/
	void tunePIDController(int _gpu, double _kp, double _ki, double _kd)
	{
		m_miners.at(_gpu)->tunePIDController(_kp, _ki, _kd);
	}

	/**
	 * @brief Called from a Miner to note a WorkPackage has a solution.
	 * @return true if the solution was good and the Farm should pause until more work is submitted.
	 */
	 /*-----------------------------------------------------------------------------------
	 * submitProof
	 *----------------------------------------------------------------------------------*/
	bool submitProof(Solution const& _s, Miner* _m)
	{
		// a miner is notifying us it found a solution.  we in turn notify the main loop 
		// (typically a lambda expression) which submits the solution to the node. the main
		// loop will call us back on solutionFound to let us know if the solution was accepted.
		LogF << "Trace: GenericFarm.submitProof";
		if (m_onSolutionFound && m_onSolutionFound(_s, _m->index()))
		{
			if (x_minerWork.try_lock())
			{
				for (auto const& m : m_miners)
					if (m != _m)
						m->setWork();
				m_work.reset();
				x_minerWork.unlock();
				return true;
			}
		}
		return false;
	}


	/**
	* @brief Called from a Miner to note a WorkPackage has a solution.
	* @return true if the solution was good and the Farm should pause until more work is submitted.
	*/
	/*-----------------------------------------------------------------------------------
	* submitProof
	*----------------------------------------------------------------------------------*/
	bool submitProof(h256 _nonce, Miner* _m) 
	{
		// a miner is notifying us it found a solution.  we in turn notify the main loop 
		// (typically a lambda expression) which submits the solution to the node. the main
		// loop will call us back on solutionFound to let us know if the solution was accepted.
		LogF << "Trace: GenericFarm.submitProof";
		if (m_onSolutionFoundToken && m_onSolutionFoundToken(_nonce, _m->index())) {
			if (x_minerWork.try_lock()) {
				challenge.clear();
				for (auto const& m : m_miners)
					if (m != _m)
						m->setWork_token(challenge, target);
				x_minerWork.unlock();
				return true;
			}
		}
		return false;
	}



public:
	unsigned currentBlock;
	DataLogger logger;

	// this value will be compared directly to the upper 64 bits of the hash. 
	uint64_t closeHitThreshold = 0;

	// this value is in seconds -- desired close hit frequency for work units. each 
	// miner needs to convert it to a hash value based on their own hash rate.
	int workUnitFreq = 0;

	// this will get set true in case of thermal runaway.
	bool shutDown = false;

	// with solo mining, the userAcct and the hashingAcct are the same.
	// with pool mining, the hashingAcct will be the pool ETH address, and
	// the userAcct will be the user's ETH address. the userAcct is private to FarmClient.h
	string hashingAcct;		// account used as part of the keccak256_0xBitcoin() function

private:
	mutable SharedMutex x_minerWork;
	miners_t m_miners;
	WorkPackage m_work;
	h256 target;
	bytes challenge;

	std::atomic<bool> m_isMining = {false};

	friend class HashRates;
	HashRates* m_hashRates;

	// event functions
	SolutionFound m_onSolutionFound;
	SolutionFoundToken m_onSolutionFoundToken;
	BestHashFn m_onBestHash;
	SetWorkFn m_onSetWork;	
	SolutionProcessedFn m_onSolutionProcessed;
	CloseHitFn m_onCloseHit;
	HashFaultFn m_onHashFault;
	// hash faults per miner for this session
	std::vector<int> m_hashFaults;
	uint64_t m_bestHash;
	mutable SharedMutex x_bestHash;
	mutable SharedMutex x_solutionStats;
	mutable SolutionStats m_solutionStats;
	// this includes work units
	unsigned m_closeHits = 0;
	// this includes work units
	uint64_t m_lastCloseHit;
}; 

}
}
