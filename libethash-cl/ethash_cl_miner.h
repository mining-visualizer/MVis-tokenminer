#pragma once

#define __CL_ENABLE_EXCEPTIONS
#define CL_USE_DEPRECATED_OPENCL_2_0_APIS

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "CL/cl.hpp"
#pragma clang diagnostic pop
#else
#include "CL/cl.hpp"
#endif

#include <time.h>
#include <functional>
#include <random>
#include <libethash/ethash.h>
#include <libdevcore/Guards.h>
#include "libethcore/EthashGPUMiner.h"

using namespace dev;

class ethash_cl_miner
{
private:
	enum { c_maxSearchResults = 63, c_bufferCount = 2, c_hashBatchSize = 1024 };

public:

	struct search_hook
	{
		virtual ~search_hook(); // always a virtual destructor for a class with virtuals.

		// reports progress, return true to abort
		virtual bool found(h256 const* nonces, uint32_t count) = 0;
		virtual bool searched(uint32_t _count, uint64_t _hashSample, uint64_t _bestHash) = 0;
		virtual bool shouldStop() = 0;
	};

	typedef struct
	{
		h256 nonce;
		unsigned buf;
	} pending_batch;


	ethash_cl_miner(eth::EthashGPUMiner* _owner);
	~ethash_cl_miner();

	static bool searchForAllDevices(unsigned _platformId, std::function<bool(cl::Device const&)> _callback);
	static bool searchForAllDevices(std::function<bool(cl::Device const&)> _callback);
	static unsigned getNumPlatforms();
	static unsigned getNumDevices(unsigned _platformId = 0);
	static std::string platform_info(unsigned _platformId = 0, unsigned _deviceId = 0);
	static void listDevices();
	static bool configureGPU(
		unsigned _platformId,
		unsigned _localWorkSize,
		unsigned _globalWorkSize,
		bool _allowCPU,
		unsigned _extraGPUMemory,
		uint64_t _currentBlock
	);

	bool verifyHashes();
	void testHashes();
	bool buildBinary(cl::Device& _device, std::string &_outfile);
	bool init(unsigned _platformId, unsigned _deviceId, h160 _miningAccount);
	void exportDAG(std::string _seedhash);
	void generateDAG(uint32_t nodes);
	bool verifyDAG(ethash_light_t _light, uint32_t _nodes);
	void finish();
	void search(bytes challenge, uint64_t _target, search_hook& _hook);
	void setThrottle(int _percent);
	void checkThrottleChange(int& _throttle, int& _bufferCount);
	uint64_t nextNonceIndex(uint64_t &_nonceIndex, bool _overrideRandom);

	/* -- default values -- */
	/// Default value of the local work size. Also known as workgroup size.
	static unsigned const c_defaultLocalWorkSize;
	/// Default value of the work size multiplier
	static unsigned const c_defaultWorkSizeMultiplier;

	eth::EthashGPUMiner* m_owner;
	uint64_t m_bestHash = ~uint64_t(0);
	mutable Mutex x_bestHash;

private:

	struct search_results
	{
		uint32_t solutions[c_maxSearchResults + 1];
		uint64_t hashes[2];		// 0=random hash, 1=best hash
	};

	static std::vector<cl::Device> getDevices(std::vector<cl::Platform> const& _platforms, unsigned _platformId);
	static std::vector<cl::Platform> getPlatforms();

	cl::Context m_context;
	cl::Program m_programCL, m_programBin;
	cl::CommandQueue m_queue[c_bufferCount];
	cl::Kernel m_searchKernel;
	cl::Kernel m_dagKernel;
	cl::Buffer m_dag;
	cl::Buffer m_light;
	cl::Buffer m_header;
	cl::Buffer m_searchBuffer[c_bufferCount];
	cl::Buffer m_nonceBuffer[c_bufferCount];
	cl::Buffer m_bestHashBuff;
	cl::Buffer m_challenge, m_sender, m_buff;
	unsigned m_globalWorkSize;
	bool m_openclOnePointOne;

	deque<pending_batch> m_pending;
	unsigned m_buf = 0;
	cl::Event m_mapEvents[c_bufferCount];		// events used for enqueueMapBuffer
	search_results* m_results[c_bufferCount];

	/// The local work size for the search
	static unsigned s_workgroupSize;
	/// The initial global work size for the searches
	static unsigned s_initialGlobalWorkSize;
	/// The target milliseconds per batch for the search. If 0, then no adjustment will happen
	static unsigned s_msPerBatch;
	/// Allow CPU to appear as an OpenCL device or not. Default is false
	static bool s_allowCPU;
	/// GPU memory required for other things, like window rendering e.t.c.
	/// User can set it via the --cl-extragpu-mem argument.
	static unsigned s_extraRequiredGPUMem;
	// percent throttling.
	int m_throttle = 0;
	mutable SharedMutex x_throttle;
	unsigned m_device;
	// random nonces
	int m_nonceGeneration;
	std::mt19937 m_randGen;
	std::uniform_int_distribution<uint64_t> m_randDist;

};
