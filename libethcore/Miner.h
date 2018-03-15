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
/** @file Miner.h
 * @author Gav Wood <i@gavwood.com>
 * @date 2015
 */

#pragma once

#include <thread>
#include <list>
#include <atomic>
#include <string>
#include <boost/timer.hpp>
#include <libdevcore/Common.h>
#include <libdevcore/Log.h>
#include <libdevcore/Worker.h>
#include <libethcore/Common.h>
#include <ethminer/Common.h>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <ethminer/MultiLog.h>
#include <ethminer/ADLUtils.h>
#ifdef _WIN32
#include <ethminer/speedfan.h>
#endif
#include "ethminer/ProgOpt.h"
#include "ethminer/Misc.h"

#define MINER_WAIT_STATE_UNKNOWN 0
#define MINER_WAIT_STATE_WORK	 1
#define MINER_WAIT_STATE_DAG	 2


#define DAG_LOAD_MODE_PARALLEL	 0
#define DAG_LOAD_MODE_SEQUENTIAL 1
#define DAG_LOAD_MODE_SINGLE	 2

#define STRATUM_PROTOCOL_STRATUM		 0
#define STRATUM_PROTOCOL_ETHPROXY		 1
#define STRATUM_PROTOCOL_ETHEREUMSTRATUM 2

#define TEMP_SOURCE_AMD			"amd_adl"
#define TEMP_SOURCE_SPEEDFAN	"speedfan"

using namespace std;

typedef struct {
	string host;
	string port;
	string user;
	string pass;
} cred_t;

namespace dev
{

namespace eth
{

enum class MinerType
{
	Undefined,
	CPU,
	CL,
	CUDA,
	Mixed
};

enum class OperationMode
{
	None,
	Benchmark,
	Solo,
	Pool
};

enum SolutionState
{
	Accepted = 1,
	Rejected = 2,
	Failed = 3
};


/*-----------------------------------------------------------------------------------
* class SolutionStats
*----------------------------------------------------------------------------------*/

class SolutionStats {
public:
	void accepted() { accepts++;  }
	void rejected() { rejects++;  }
	void failed()   { failures++; }

	void acceptedStale() { acceptedStales++; }
	void rejectedStale() { rejectedStales++; }


	void reset() { accepts = rejects = failures = acceptedStales = rejectedStales = 0; }

	unsigned getAccepts()			{ return accepts; }
	unsigned getRejects()			{ return rejects; }
	unsigned getFailures()			{ return failures; }
	unsigned getAcceptedStales()	{ return acceptedStales; }
	unsigned getRejectedStales()	{ return rejectedStales; }
private:
	unsigned accepts  = 0;
	unsigned rejects  = 0;
	unsigned failures = 0; 

	unsigned acceptedStales = 0;
	unsigned rejectedStales = 0;

};	 // class SolutionStats



inline std::ostream& operator<<(std::ostream& os, SolutionStats s)
{
	return os << "[A" << s.getAccepts() << "+" << s.getAcceptedStales() << ":R" << s.getRejects() << "+" << s.getRejectedStales() << ":F" << s.getFailures() << "]";
}


template <class PoW> class GenericMiner;

template <class PoW> class GenericFarm;



/*-----------------------------------------------------------------------------------
* class PIDController
*----------------------------------------------------------------------------------*/

// implement GPU throttling using a PID controller
template <class PoW> class PIDController
{

private:
	// note: the tuning parameters below have been tailored to an update rate of 
	// 2000 ms.  if you change the update rate, you have to re-tune the PID controller.
	enum { c_updateRate = 2000 };

public:

	PIDController(GenericMiner<PoW>& _miner) : m_miner(_miner) {

		// don't try to do anything with _miner here, because it has not yet been fully constructed.
		LogF << "Trace: PIDController::constructor";
		m_lastUpdate.restart();

		TimerCallback::Init();
		m_updateTimer = new TimerCallback(bind(&PIDController::update, this, _1));
		m_updateTimer->start(c_updateRate);

		setPoint = strToInt(ProgOpt::Get("ThermalProtection", "ThrottleTemp", "80"), 80);
		shutDownTime = strToInt(ProgOpt::Get("ThermalProtection", "ShutDown", "20"), 20);
	}

	void update(void* _unused)
	{
		if (setPoint < 0) return;

		double gpuTemp = m_miner.gpuTemp();

		// check for thermal runaway

		// m_thermalRunaway is a cumulative measure of the number of milliseconds we have been in a thermal
		// condition. it counts up when we're over the limit, and it counts down when we are under, but at a 
		// reduced rate.
		double threshold = m_thermalRunaway > 0 ? 0.75 : 0.0;
		if (gpuTemp > (setPoint - threshold))
		{
			m_thermalRunaway += c_updateRate;
			if (m_thermalRunaway > (shutDownTime * 1000))
			{
				// affect shutdown
				LogB << " ";
				LogB << "Thermal Limits Exceeded!!  Mining Terminated!!";
				m_miner.m_farm->shutDown = true;
			}
		}
		else
			m_thermalRunaway = std::max(0, m_thermalRunaway - int(c_updateRate * 0.75));

		// PID calculations
		double error = gpuTemp - setPoint;
		m_iTerm += Ki * (error * m_lastUpdate.elapsedSeconds());
		// stop summing the integral once it has pushed past the limits
		m_iTerm = min(100.0, max(0.0, m_iTerm));
		double derivative = (error - m_prevError) / m_lastUpdate.elapsedSeconds();
		int throttle = Kp * error + m_iTerm + Kd * derivative;
		throttle = min(100, max(0, throttle));
		m_miner.setThrottle(throttle);

		m_lastUpdate.restart();
		m_prevError = error;
		MultiLog(LogFiltered, LogFiltered) << "PIDCtrl: ," << setPoint << ", " << gpuTemp << ", " << error << ", " << m_iTerm << ", " << derivative << ", " << throttle;
	}

	// negative values mean "no change"
	void tune(double _kp, double _ki, double _kd)
	{
		Kp = _kp < 0 ? Kp : _kp;
		Ki = _ki < 0 ? Ki : _ki;
		Kd = _kd < 0 ? Kd : _kd;
		LogF << "PIDController.tune : Kp = " << Kp << ", Ki = " << Ki << ", Kd = " << Kd;
	}

public:

	double setPoint;		// set to negative to disable PIDController
	int shutDownTime;		// number of seconds thermal runaway will be tolerated until we shut down.
	double Kp = 8;
	double Ki = 4;
	double Kd = 1;

private:
	GenericMiner<PoW>& m_miner;
	Timer m_lastUpdate;
	double m_iTerm = 0;
	double m_prevError = 0;
	TimerCallback* m_updateTimer;
	// see comments above.
	int m_thermalRunaway = -1;

};	// class PIDController


/*-----------------------------------------------------------------------------------
* class GenericMiner
*----------------------------------------------------------------------------------*/

/**
 * @brief A miner - a member and adoptee of the Farm.
 * @warning Not threadsafe. It is assumed Farm will synchronise calls to/from this class.
 */
template <class PoW> class GenericMiner
{
public:
	using WorkPackage = typename PoW::WorkPackage;
	using Solution = typename PoW::Solution;
	using Farm = GenericFarm<PoW>;


	GenericMiner(Farm* _farm, unsigned _index):
		m_farm(_farm), m_index(_index), m_pidController(*this)
	{
		m_tempSource = ProgOpt::Get("ThermalProtection", "TempProvider", "amd_adl");
		LowerCase(m_tempSource);
	}

	virtual ~GenericMiner() {}

	// API FOR THE FARM TO CALL IN WITH

	void setWork(WorkPackage const& _work = WorkPackage())
	{
		LogF << "Trace: GenericMiner::setWork, miner[" << m_index << "]";
		auto old = m_work;
		{
			Guard l(x_work);
			m_work = _work;
		}
		if (!!_work)
		{
			DEV_TIMED_ABOVE("pause", 250)
				pause();
			DEV_TIMED_ABOVE("kickOff", 250)
				kickOff();
		}
		else if (!_work && !!old)
			pause();

		if (m_index == 0)
			// clear out the nonces. only one miner needs to do this.
			storeNonceIndex(0, true);

		//  we'll use this as a convenient place to recalculate our work unit threshold periodically
		calcWorkUnitThreshold();
	}

	void setWork_token(bytes _challenge, h256 _target) 
	{
		LogF << "Trace: GenericMiner::setWork, miner[" << m_index << "]";
		auto old = challenge;
		{
			Guard l(x_work);
			challenge = _challenge;
			target = _target;
		}
		if (!_challenge.empty()) {
			DEV_TIMED_ABOVE("pause", 250)
				pause();
			DEV_TIMED_ABOVE("kickOff", 250)
				kickOff();
		} else if (_challenge.empty() && !old.empty())
			pause();

		if (m_index == 0)
			// clear out the nonces. only one miner needs to do this.
			storeNonceIndex(0, true);

		//  we'll use this as a convenient place to recalculate our work unit threshold periodically
		calcWorkUnitThreshold();
	}


	void calcWorkUnitThreshold()
	{
		// calculate a threshold for work units. MVis gives us the desired number of 
		// seconds between close hits, and we convert that to a hash value based on our hash rate.
		float rate = m_farm->hashRates().minerRate(m_index);
		double divisor = rate * 1000000.0 * m_farm->workUnitFreq;
		uint64_t workUnitFrequency = (divisor == 0) ? 0 : ~uint64_t(0) / divisor;

		// compute overall threshold for close hits, taking into account close hits destined for the desktop widget
		// display (which are usually set at a much higher difficulty level), and close hits used for work units.
		m_closeHit = max(workUnitFrequency, m_farm->closeHitThreshold);

		LogF << "Trace: GenericMiner::calcWorkUnitThreshold :"
			<< " m_closeHit = " << m_closeHit
			<< ", rate = " << rate
			<< ", workUnitFrequency = " << workUnitFrequency
			<< ", closeHitThreshold = " << m_farm->closeHitThreshold;
	}


	/**
	*   @brief Get the hash rate.
	*/
	double getHashRate()
	{
		ReadGuard l(x_hashRates);
		LogF << "Trace: GenericMiner::getHashRate, miner = " << m_index << ", hashRate = " << m_hashRate.value();
		return m_hashRate.value();
	}

	uint64_t currentHash() 
	{ 
		ReadGuard l(x_hashVal); 
		return m_currentHash; 
	}

	void setCurrentHash(uint64_t _hash) 
	{
		WriteGuard l(x_hashVal);
		m_currentHash = _hash;
	}

	/**
	*   @brief Miner is providing its best hash
	*/
	void setBestHash(uint64_t _bh)
	{
		m_farm->suggestBestHash(_bh);
		WriteGuard l(x_hashVal);
		m_bestHash = _bh;
	}

	void setBestHash(h256 _bh) 
	{ 
		setBestHash(upper64OfHash(_bh)); 
	}

	uint64_t bestHash() 
	{ 
		ReadGuard l(x_hashVal); 
		return m_bestHash; 
	}

	virtual void resetBestHash() 
	{ 
		WriteGuard l(x_hashVal); 
		m_bestHash = ~uint64_t(0); 
	}

	unsigned index() const 
	{ 
		return m_index; 
	}

	// functionality implemented in descendant classes.
	virtual void setThrottle(int _percent) { }

	int throttle(void) 
	{ 
		return m_throttle; 
	}

	double gpuTemp(void)
	{
		if (m_tempSource == TEMP_SOURCE_SPEEDFAN)
		{
			#ifdef _WIN32
			std::vector<double> data;
			g_SpeedFan.getData(data, SpeedFan::Temperatures, m_index + 1);
			return data.size() > m_index ? data[m_index] : 0;
			#else
			return 0;
			#endif
		}
		else
			return g_ADLUtils.getTemps(m_device);
	}

	void thermalProtection(int _maxTemp, double _shutdown)
	{
		m_pidController.setPoint = _maxTemp;
		m_pidController.shutDownTime = _shutdown;
	}
	
	void tunePIDController(double _kp, double _ki, double _kd)
	{
		m_pidController.tune(_kp, _ki, _kd);
	}

	int fanSpeed(void)
	{
		return g_ADLUtils.getFanSpeed(m_device);
	}

	/**
	* @brief return current work package.
	*/
	WorkPackage const& work() const 
	{ 
		Guard l(x_work); 
		return m_work; 
	}


	bool storeNonceIndex(uint64_t _nonceIndex, bool _clear = false)
	{
		// when nonces are being generated randomly, we keep track of which ones
		// have been searched so as to avoid repetition.  s_usedNonces is static so
		// it is shared across all miners.
		static set<unsigned> s_usedNonces;
		static dev::SpinLock x_nonces;
		dev::SpinGuard l(x_nonces);
		if (_clear)
		{
			s_usedNonces.clear();
			return true;
		}
		else
		{
			// return true if this nonce index has not already been stored.
			auto result = s_usedNonces.insert(_nonceIndex);
			return result.second;
		}
	}

	// member element in m_recentHashes
	typedef struct
	{
		SystemClock::time_point t;
		int hashes;
	} hashRec_t;

	/**
	* @brief record # of hashes computed.
	*/
	void accumulateHashes(unsigned _n, int _batchCount)
	{

		if (_batchCount < 2)
		{
			// we ignore the first few batches.  when cl_miner.search() starts out, after a new
			// work package has arrived, it experiences an extra delay because it has to wait for 
			// the last kernel run from the previous work package to finish.
			m_hashCount = 0;
			m_hashTimer.restart();
			return;
		}
		m_hashCount += _n;
		uint64_t elapsed = m_hashTimer.elapsedMicroseconds();
		// we'll accumulate hashes for awhile and then update our exponential moving average.
		if (elapsed / 1000 > 700)
		{
			double batchRate = (double) m_hashCount / elapsed;
			m_hashRate.newVal(batchRate);
			m_hashCount = 0;
			m_hashTimer.restart();
			LogF << "accumulateHashes.update: batch rate = " << batchRate << ", hash rate = " << m_hashRate.value();
		}
		else
		{
			LogF << "accumulateHashes.accumulate: hashes = " << m_hashCount << ", time = " << elapsed / 1000;
		}
	}	// accumulateHashes

public:
	GenericFarm<PoW>* m_farm = nullptr;

protected:


	// REQUIRED TO BE REIMPLEMENTED BY A SUBCLASS:

	/**
	 * @brief Begin working on a given work package, discarding any previous work.
	 * @param _work The package for which to find a solution.
	 */
	virtual void kickOff() = 0;

	/**
	 * @brief No work left to be done. Pause until told to kickOff().
	 */
	virtual void pause() = 0;

	/**
	 * @brief Notes that the Miner found a solution.
	 * @param _s The solution.
	 * @return true if the solution was correct and that the miner should pause.
	 */
	bool submitProof(Solution const& _s)
	{
		LogF << "Trace: GenericMiner::submitProof, miner[" << m_index << "]";
		if (!m_farm)
			return true;
		if (m_farm->submitProof(_s, this))
		{
			Guard l(x_work);
			m_work.reset();
			return true;
		}
		return false;
	}

	/**
	* @brief Notes that the Miner found a solution.
	* @param _s The solution.
	* @return true if the solution was correct and that the miner should pause.
	*/
	bool submitProof(h256 _nonce) 
	{
		LogF << "Trace: GenericMiner::submitProof, miner[" << m_index << "]";
		if (!m_farm)
			return true;
		if (m_farm->submitProof(_nonce, this)) {
			Guard l(x_work);
			challenge.clear();
			return true;
		}
		return false;
	}


	mutable SharedMutex x_hashVal;
	uint64_t m_currentHash;
	uint64_t m_bestHash;
	uint64_t m_closeHit = 0; 
	SteadyClock::time_point m_lastCloseHit = SteadyClock::now();

	static unsigned s_dagLoadMode;
	static volatile unsigned s_dagLoadIndex;
	static unsigned s_dagCreateDevice;
	static volatile void* s_dagInHostMemory;
	int m_throttle = 0;
	unsigned m_index;		// zero-based
	unsigned m_device;

	h256 target;
	bytes challenge;

private:

	uint64_t m_hashCount = 0;
	Timer m_hashTimer;
	mutable SharedMutex x_hashRates;
	EMA m_hashRate = EMA(4);
	// start time of hash rate accumulation period
	SteadyClock::time_point m_hashPeriodStart;
	bool m_hashRatePaused = true;

	WorkPackage m_work;
	mutable Mutex x_work;

	bool m_dagLoaded = false;
	std::string m_tempSource;

	PIDController<PoW> m_pidController;

};


}
}
