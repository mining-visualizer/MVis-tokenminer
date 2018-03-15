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
/** @file EthashCUDAMiner.cpp
* @author Gav Wood <i@gavwood.com>
* @date 2014
*
* Determines the PoW algorithm.
*/

#if ETH_ETHASHCUDA || !ETH_TRUE

#include "EthashCUDAMiner.h"
#include <thread>
#include <chrono>
#include <libethash-cuda/ethash_cuda_miner.h>

#if defined(WIN32)
#include <Windows.h>
#endif

using namespace std;
using namespace dev;
using namespace eth;

namespace dev
{
namespace eth
{
	class EthashCUDAHook : public ethash_cuda_miner::search_hook
	{
	public:

		EthashCUDAHook(EthashCUDAMiner* _owner) : m_owner(_owner) {}
		EthashCUDAHook(EthashCUDAHook const&) = delete;

		void abort()
		{
			{
				UniqueGuard l(x_all);
				if (m_aborted)
					return;
				//		cdebug << "Attempting to abort";

				m_abort = true;
			}
			// m_abort is true so now searched()/found() will return true to abort the search.
			// we hang around on this thread waiting for them to point out that they have aborted since
			// otherwise we may end up deleting this object prior to searched()/found() being called.
			m_aborted.wait(true);
			//		for (unsigned timeout = 0; timeout < 100 && !m_aborted; ++timeout)
			//			std::this_thread::sleep_for(chrono::milliseconds(30));
			//		if (!m_aborted)
			//			cwarn << "Couldn't abort. Abandoning OpenCL process.";
		}

		void reset()
		{
			UniqueGuard l(x_all);
			m_aborted = m_abort = false;
		}

	protected:
		virtual bool found(uint64_t const* _nonces, uint32_t _count) override
		{
			//		dev::operator <<(std::cerr << "Found nonces: ", vector<uint64_t>(_nonces, _nonces + _count)) << std::endl;
			for (uint32_t i = 0; i < _count; ++i)
				if (m_owner->report(_nonces[i]))
					return (m_aborted = true);
			return m_owner->shouldStop();
		}

		virtual bool searched(uint32_t _count, uint64_t _hashSample, uint64_t _bestHash) override
		{
			UniqueGuard l(x_all);
			bool shouldStop = m_abort || m_owner->shouldStop();
			m_owner->accumulateHashes(_count);
			m_owner->setCurrentHash(_hashSample);
			m_owner->setBestHash(_bestHash);	// this best hash might be the same as the last one, but we're not time critical here.
			return (m_aborted = shouldStop);
		}

	private:
		Mutex x_all;
		bool m_abort = false;
		Notified<bool> m_aborted = { true };
		EthashCUDAMiner* m_owner = nullptr;
	};
}
}

unsigned EthashCUDAMiner::s_platformId = 0;
unsigned EthashCUDAMiner::s_deviceId = 0;
unsigned EthashCUDAMiner::s_numInstances = 0;
int EthashCUDAMiner::s_devices[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

EthashCUDAMiner::EthashCUDAMiner(Farm* _farm, unsigned _index) :
	GenericMiner<EthashProofOfWork>(_farm, _index),
	Worker("cudaminer" + toString(index())),
m_hook( new EthashCUDAHook(this))
{
}

EthashCUDAMiner::~EthashCUDAMiner()
{
	pause();
	delete m_miner;
	delete m_hook;
}

bool EthashCUDAMiner::report(uint64_t _nonce)
{
	Nonce n = (Nonce)(u64)_nonce;
	EthashProofOfWork::Result r = EthashAux::eval(work().seedHash, work().headerHash, n);
	if (r.value < work().boundary)
		return submitProof(Solution{ n, r.mixHash });
	return false;
}

void EthashCUDAMiner::kickOff()
{
	m_hook->reset();
	startWorking();
}

void EthashCUDAMiner::workLoop()
{
	// take local copy of work since it may end up being overwritten by kickOff/pause.
	try {
		WorkPackage w = work();
		//cnote << "set work; seed: " << "#" + w.seedHash.hex().substr(0, 8) + ", target: " << "#" + w.boundary.hex().substr(0, 12);
		if (!m_miner || m_minerSeed != w.seedHash)
		{
			m_device = s_devices[index()] > -1 ? s_devices[index()] : index();

			if (s_dagLoadMode == DAG_LOAD_MODE_SEQUENTIAL)
			{
				while (s_dagLoadIndex < index()) {
					this_thread::sleep_for(chrono::seconds(1));
				}
			}
			else if (s_dagLoadMode == DAG_LOAD_MODE_SINGLE)
			{
				if (m_device != s_dagCreateDevice)
				{
					// wait until DAG is created on selected device
					while (s_dagInHostMemory == NULL) {
						this_thread::sleep_for(chrono::seconds(1));
					}
				}
				else
				{
					// reset load index
					s_dagLoadIndex = 0;
				}
			}

			LogS << "Initialising miner ...";
			m_minerSeed = w.seedHash;

			delete m_miner;
			m_miner = new ethash_cuda_miner;

			EthashAux::LightType light;
			light = EthashAux::light(w.seedHash);
			//bytesConstRef dagData = dag->data();
			bytesConstRef lightData = light->data();
			
			m_miner->init(light->light, lightData.data(), lightData.size(), m_device, (s_dagLoadMode == DAG_LOAD_MODE_SINGLE), &s_dagInHostMemory);
			s_dagLoadIndex++;

			if (s_dagLoadMode == DAG_LOAD_MODE_SINGLE)
			{
				if (s_dagLoadIndex >= s_numInstances && s_dagInHostMemory)
				{
					// all devices have loaded DAG, we can free now
					delete[] s_dagInHostMemory;
					s_dagInHostMemory = NULL;

					cout << "Freeing DAG from host" << endl;
				}
			}
		}

		m_farm->setIsMining(true);

		uint64_t startN = 0;
		if (w.exSizeBits >= 0)
			startN = w.startNonce | ((uint64_t)index() << (64 - 4 - w.exSizeBits)); // this can support up to 16 devices
		m_miner->search(w.headerHash.data(), upper64OfHash(w.boundary), *m_hook, (w.exSizeBits >= 0), startN);
	}
	catch (std::runtime_error const& _e)
	{
		delete m_miner;
		m_miner = nullptr;
		cwarn << "Error CUDA mining: " << _e.what();
	}
}

void EthashCUDAMiner::pause()
{
	m_hook->abort();
	stopWorking();
}

std::string EthashCUDAMiner::platformInfo()
{
	return ethash_cuda_miner::platform_info(s_deviceId);
}

unsigned EthashCUDAMiner::getNumDevices()
{
	return ethash_cuda_miner::getNumDevices();
}

void EthashCUDAMiner::listDevices()
{
	return ethash_cuda_miner::listDevices();
}

bool EthashCUDAMiner::configureGPU(
	unsigned _blockSize,
	unsigned _gridSize,
	unsigned _numStreams,
	unsigned _extraGPUMemory,
	unsigned _scheduleFlag,
	uint64_t _currentBlock,
	unsigned _dagLoadMode,
	unsigned _dagCreateDevice
	)
{
	s_dagLoadMode = _dagLoadMode;
	s_dagCreateDevice = _dagCreateDevice;
	_blockSize = ((_blockSize + 7) / 8) * 8;

	if (!ethash_cuda_miner::configureGPU(
		s_devices,
		_blockSize,
		_gridSize,
		_numStreams,
		_extraGPUMemory,
		_scheduleFlag,
		_currentBlock)
		)
	{
		cout << "No CUDA device with sufficient memory was found. Can't CUDA mine. Remove the -U argument" << endl;
		return false;
	}
	return true;
}

#endif