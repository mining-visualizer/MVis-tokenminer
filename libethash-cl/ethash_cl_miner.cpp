/*
  This file is part of c-ethash.

  c-ethash is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  c-ethash is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file ethash_cl_miner.cpp
* @author Tim Hughes <tim@twistedfury.com>
* @date 2015
*/


#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <assert.h>
#include <queue>
#include <vector>
#include <atomic>
#include <sstream>
#include <libethash/util.h>
#include <libethash/ethash.h>
#include <libethash/internal.h>
#include "ethash_cl_miner.h"
#include "ethash_cl_miner_kernel.h"
#include <libdevcore/Log.h>
#include "ethminer/MultiLog.h"
#include "ethminer/Misc.h"
#include <libethash/sha3_cryptopp.h>

#define ETHASH_BYTES 32

#define OPENCL_PLATFORM_UNKNOWN 0
#define OPENCL_PLATFORM_NVIDIA  1
#define OPENCL_PLATFORM_AMD		2

// workaround lame platforms
#if !CL_VERSION_1_2
#define CL_MAP_WRITE_INVALIDATE_REGION CL_MAP_WRITE
#define CL_MEM_HOST_READ_ONLY 0
#endif

// apple fix
#ifndef CL_DEVICE_COMPUTE_CAPABILITY_MAJOR_NV
#define CL_DEVICE_COMPUTE_CAPABILITY_MAJOR_NV       0x4000
#endif

#ifndef CL_DEVICE_COMPUTE_CAPABILITY_MINOR_NV
#define CL_DEVICE_COMPUTE_CAPABILITY_MINOR_NV       0x4001
#endif

#undef min
#undef max

using namespace std;
using namespace dev;

unsigned const ethash_cl_miner::c_defaultLocalWorkSize = 128;
unsigned const ethash_cl_miner::c_defaultWorkSizeMultiplier = 196608;

unsigned const c_nonceLinear = 1;
unsigned const c_nonceRandom = 2;

uint32_t const c_zero = 0;
uint64_t const c_maxHash = ~uint64_t(0);

// static initializers
bool ethash_cl_miner::s_allowCPU = false;
unsigned ethash_cl_miner::s_extraRequiredGPUMem;
unsigned ethash_cl_miner::s_workgroupSize = ethash_cl_miner::c_defaultLocalWorkSize;
unsigned ethash_cl_miner::s_initialGlobalWorkSize = ethash_cl_miner::c_defaultWorkSizeMultiplier * ethash_cl_miner::c_defaultLocalWorkSize;


// TODO: If at any point we can use libdevcore in here then we should switch to using a LogChannel
#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* lpOutputString);
static std::atomic_flag s_logSpin = ATOMIC_FLAG_INIT;
#define ETHCL_LOG(_contents) \
	do \
	{ \
		std::stringstream ss; \
		ss << _contents; \
		while (s_logSpin.test_and_set(std::memory_order_acquire)) {} \
		OutputDebugStringA(ss.str().c_str()); \
		cerr << ss.str() << endl << flush; \
		s_logSpin.clear(std::memory_order_release); \
	} while (false)
#else
#define ETHCL_LOG(_contents) cout << "[OPENCL]:" << _contents << endl
#endif

// Types of OpenCL devices we are interested in
#define ETHCL_QUERIED_DEVICE_TYPES (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR)

static void addDefinition(string& _source, char const* _id, unsigned _value)
{
	char buf[256];
	sprintf(buf, "#define %s %uu\n", _id, _value);
	_source.insert(_source.begin(), buf, buf + strlen(buf));
}

ethash_cl_miner::search_hook::~search_hook() {}

ethash_cl_miner::ethash_cl_miner(eth::EthashGPUMiner* _owner)
	: m_openclOnePointOne(), m_owner(_owner)
{
	if (ProgOpt::Get("General", "NonceGeneration") == "Random")
		m_nonceGeneration = c_nonceRandom;
	else
		m_nonceGeneration = c_nonceLinear;
}

ethash_cl_miner::~ethash_cl_miner()
{
	finish();
}

std::vector<cl::Platform> ethash_cl_miner::getPlatforms()
{
	vector<cl::Platform> platforms;
	try
	{
		cl::Platform::get(&platforms);
	}
	catch(cl::Error const& err)
	{
#if defined(CL_PLATFORM_NOT_FOUND_KHR)
		if (err.err() == CL_PLATFORM_NOT_FOUND_KHR)
			LogS << "No OpenCL platforms found";
		else
#endif
			LogS << err.what();
	}
	return platforms;
}

string ethash_cl_miner::platform_info(unsigned _platformId, unsigned _deviceId)
{
	vector<cl::Platform> platforms = getPlatforms();
	if (platforms.empty())
		return {};
	// get GPU device of the selected platform
	unsigned platform_num = min<unsigned>(_platformId, platforms.size() - 1);
	vector<cl::Device> devices = getDevices(platforms, _platformId);
	if (devices.empty())
	{
		LogS << "No OpenCL devices found.";
		return {};
	}

	// use selected default device
	unsigned device_num = min<unsigned>(_deviceId, devices.size() - 1);
	cl::Device& device = devices[device_num];
	string device_version = device.getInfo<CL_DEVICE_VERSION>();

	return "{ \"platform\": \"" + platforms[platform_num].getInfo<CL_PLATFORM_NAME>() + "\", \"device\": \"" + device.getInfo<CL_DEVICE_NAME>() + "\", \"version\": \"" + device_version + "\" }";
}

std::vector<cl::Device> ethash_cl_miner::getDevices(std::vector<cl::Platform> const& _platforms, unsigned _platformId)
{
	vector<cl::Device> devices;
	unsigned platform_num = min<unsigned>(_platformId, _platforms.size() - 1);
	try
	{
		_platforms[platform_num].getDevices(
			s_allowCPU ? CL_DEVICE_TYPE_ALL : ETHCL_QUERIED_DEVICE_TYPES,
			&devices
		);
	}
	catch (cl::Error const& err)
	{
		// if simply no devices found return empty vector
		if (err.err() != CL_DEVICE_NOT_FOUND)
			LogS << err.what();
	}
	return devices;
}

unsigned ethash_cl_miner::getNumPlatforms()
{
	vector<cl::Platform> platforms = getPlatforms();
	if (platforms.empty())
		return 0;
	return platforms.size();
}

unsigned ethash_cl_miner::getNumDevices(unsigned _platformId)
{
	vector<cl::Platform> platforms = getPlatforms();
	if (platforms.empty())
		return 0;

	vector<cl::Device> devices = getDevices(platforms, _platformId);
	if (devices.empty())
	{
		LogS << "No OpenCL devices found.";
		return 0;
	}
	return devices.size();
}

bool ethash_cl_miner::configureGPU(
	unsigned _platformId,
	unsigned _localWorkSize,
	unsigned _globalWorkSize,
	bool _allowCPU,
	unsigned _extraGPUMemory,
	uint64_t _currentBlock
)
{
	LogF << "Trace: ethash_cl_miner::configureGPU [1]";
	s_workgroupSize = _localWorkSize;
	s_initialGlobalWorkSize = _globalWorkSize;
	s_allowCPU = _allowCPU;
	s_extraRequiredGPUMem = _extraGPUMemory;

	// by default let's only consider the DAG of the first epoch
	uint64_t dagSize = ethash_get_datasize(_currentBlock);
	uint64_t requiredSize =  dagSize + _extraGPUMemory;
	return searchForAllDevices(_platformId, [&requiredSize](cl::Device const& _device) -> bool
		{
			cl_ulong result;
			_device.getInfo(CL_DEVICE_GLOBAL_MEM_SIZE, &result);
			if (result >= requiredSize)
			{
				//std::string s = string(_device.getInfo<CL_DEVICE_NAME>().c_str());
				//LogS << "Found suitable OpenCL device [" << s << "] with " << result << " bytes of GPU memory";
				return true;
			}

			LogB << "OpenCL device " << _device.getInfo<CL_DEVICE_NAME>()
				<< " has insufficient GPU memory." << result <<
				" bytes of memory found < " << requiredSize << " bytes of memory required";
			return false;
		}
	);
}

bool ethash_cl_miner::searchForAllDevices(function<bool(cl::Device const&)> _callback)
{
	vector<cl::Platform> platforms = getPlatforms();
	if (platforms.empty())
		return false;
	for (unsigned i = 0; i < platforms.size(); ++i)
		if (searchForAllDevices(i, _callback))
			return true;

	return false;
}

bool ethash_cl_miner::searchForAllDevices(unsigned _platformId, function<bool(cl::Device const&)> _callback)
{
	vector<cl::Platform> platforms = getPlatforms();
	if (platforms.empty())
		return false;
	if (_platformId >= platforms.size())
		return false;

	vector<cl::Device> devices = getDevices(platforms, _platformId);
	for (cl::Device const& device: devices)
		if (_callback(device))
			return true;

	return false;
}

void ethash_cl_miner::listDevices()
{
	stringstream output;

	// list platforms
	output << endl << endl;
	output << "OPENCL PLATFORMS:" << endl;
	output << "[PlatformID] Platform Name " << endl;
	output << "------------------------------------------------------" << endl;

	vector<cl::Platform> platforms = getPlatforms();
	if (platforms.empty())
		output << " -- no platforms or devices found! -- " << endl;
	else
	{
		for (unsigned p = 0; p < platforms.size(); ++p)
			output << "[" << p << "]  " << platforms[p].getInfo<CL_PLATFORM_VENDOR>() << endl;

		// list devices
		output << endl;
		output << "OPENCL DEVICES:" << endl;
		output << "[PlatformID, DeviceID] Device Name" << endl;
		output << "------------------------------------------------------" << endl;
		for (unsigned p = 0; p < platforms.size(); ++p)
		{
			vector<cl::Device> devices = getDevices(platforms, p);
			for (unsigned i = 0; i < devices.size(); ++i)
			{
				output << "[" << p << ", " << i << "] " << devices[i].getInfo<CL_DEVICE_NAME>() << endl;
				output << "\tDEVICE_TYPE: ";
				switch (devices[i].getInfo<CL_DEVICE_TYPE>())
				{
					case CL_DEVICE_TYPE_CPU:
						output << "CPU" << endl;
						break;
					case CL_DEVICE_TYPE_GPU:
						output << "GPU" << endl;
						break;
					case CL_DEVICE_TYPE_ACCELERATOR:
						output << "ACCELERATOR" << endl;
						break;
					default:
						output << "DEFAULT" << endl;
						break;
				}
				output << "\tGLOBAL_MEM_SIZE: " << devices[i].getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>() << endl;
				output << "\tMAX_MEM_ALLOC_SIZE: " << devices[i].getInfo<CL_DEVICE_MAX_MEM_ALLOC_SIZE>() << endl;
				output << "\tMAX_WORK_GROUP_SIZE: " << devices[i].getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>() << endl;
				output << "\tMAX_COMPUTE_UNITS: " << devices[i].getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>() << endl;
			}
		}
	}
	LogS << output.str();
}


void ethash_cl_miner::finish()
{
	for (int i = 0; i < c_bufferCount; i++)
		if (m_queue[i]())
			m_queue[i].finish();
}

uint64_t chi(uint64_t a, uint64_t b, uint64_t c)
{
	return a ^ ((~b) & c);
}

#define ROTL64(x, y) (((x) << (y)) ^ ((x) >> (64 - (y))))

void keccak_precomp(uint64_t* mid, uint64_t* message)
{
	uint64_t C[4], D[5];
	C[0] = message[0] ^ message[5] ^ message[10] ^ 0x100000000ull;
	C[1] = message[1] ^ message[6] ^ 0x8000000000000000ull;
	C[2] = message[2] ^ message[7];
	C[3] = message[4] ^ message[9];

	D[0] = ROTL64(C[1], 1) ^ C[3];
	D[1] = ROTL64(C[2], 1) ^ C[0];
	D[2] = ROTL64(message[3], 1) ^ C[1];
	D[3] = ROTL64(C[3], 1) ^ C[2];
	D[4] = ROTL64(C[0], 1) ^ message[3];

	mid[0] = message[0] ^ D[0];
	mid[1] = ROTL64(message[6] ^ D[1], 44);
	mid[2] = ROTL64(D[2], 43);
	mid[3] = ROTL64(D[3], 21);
	mid[4] = ROTL64(D[4], 14);
	mid[5] = ROTL64(message[3] ^ D[3], 28);
	mid[6] = ROTL64(message[9] ^ D[4], 20);
	mid[7] = ROTL64(message[10] ^ D[0] ^ 0x100000000ull, 3);
	mid[8] = ROTL64(0x8000000000000000ull ^ D[1], 45);
	mid[9] = ROTL64(D[2], 61);
	mid[10] = ROTL64(message[1] ^ D[1], 1);
	mid[11] = ROTL64(message[7] ^ D[2], 6);
	mid[12] = ROTL64(D[3], 25);
	mid[13] = ROTL64(D[4], 8);
	mid[14] = ROTL64(D[0], 18);
	mid[15] = ROTL64(message[4] ^ D[4], 27);
	mid[16] = ROTL64(message[5] ^ D[0], 36);
	mid[17] = ROTL64(D[1], 10);
	mid[18] = ROTL64(D[2], 15);
	mid[19] = ROTL64(D[3], 56);
	mid[20] = ROTL64(message[2] ^ D[2], 62);
	mid[21] = ROTL64(D[3], 55);
	mid[22] = ROTL64(D[4], 39);
	mid[23] = ROTL64(D[0], 41);
	mid[24] = ROTL64(D[1], 2);

}

void ethash_cl_miner::verifyHashes() {

	cl::Buffer precompBuff(m_context, CL_MEM_READ_ONLY, 200);
	cl::Buffer output(m_context, CL_MEM_READ_WRITE, 8);

	cl::Kernel testKeccak(m_programCL, "test_keccak");

	testKeccak.setArg(0, precompBuff);
	testKeccak.setArg(1, output);

	for (int i = 0; i != 50; ++i) {

		// fill message elements with random data
		h256 nonce = h256::random();
		bytes challenge(32);
		memcpy(challenge.data(), nonce.data(), 32);
		nonce = h256::random();
		h160 sender = h160::random();

		uint8_t message[88] = {0};

		memcpy(&message[0], challenge.data(), 32);
		memcpy(&message[32], sender.data(), 20);
		memcpy(&message[52], nonce.data(), 32);

		uint64_t precomp[25];
		keccak_precomp(precomp, (uint64_t*) message);
		m_queue[0].enqueueWriteBuffer(precompBuff, CL_TRUE, 0, 200, precomp);

		testKeccak.setArg(2, i);

		m_queue[0].enqueueNDRangeKernel(testKeccak, cl::NullRange, i + 1, s_workgroupSize);

		bytes kernelhash(8);
		m_queue[0].enqueueReadBuffer(output, CL_TRUE, 0, 8, kernelhash.data());
		m_queue[0].finish();

		// now compute the hash on the CPU host and compare
		bytes hash(32);
		((uint64_t*) message)[8] = i;	// set the gid to duplicate what the kernel does
		SHA3_256((const ethash_h256_t*) hash.data(), (uint8_t const*) message, 84);

		hash.resize(8);
		if (hash != kernelhash) {
			LogS << "Not equal!";
		}
		LogS << "CPU: " << toHex(hash);
		LogS << "GPU: " << toHex(kernelhash);
	}

}


std::string quote(std::string _s)
{
	return "\"" + _s + "\"";
}

int32_t count_leading_zeros(uint32_t val)
{

	// this code is adapted from libdivide by ridiculous_fish (http://www.libdivide.com)

	int32_t result = 0;
	uint32_t hi = 1U << 31;

	while (~val & hi)
	{
		hi >>= 1;
		result++;
	}
	return result;
}

static uint32_t makeMagic(uint32_t _shift, uint32_t _divisor, uint32_t& _rem)
{

	// this code is adapted from libdivide by ridiculous_fish (http://www.libdivide.com)

	// compute 2**(_shift+32) / _divisor
	uint32_t m = 1u << _shift;
	uint64_t n = (((uint64_t) m) << 32);
	uint32_t result = (uint32_t) (n / _divisor);
	_rem = (uint32_t) (n - result * (uint64_t) _divisor);
	return result;
}

#define LIBDIVIDE_ADD_MARKER  0x40

void DivMagic(uint32_t _dagSize128, std::string& _flags)
{

	// this code is adapted from libdivide by ridiculous_fish (http://www.libdivide.com)
	// we're setting some defines in the _flags parameter which will then be available
	// to the assembly language code. it uses this info to do efficient integer division.

	const uint32_t leadingZeros = 31 - count_leading_zeros(_dagSize128);
	int shift2;
	uint32_t rem, magic;
	magic = makeMagic(leadingZeros, _dagSize128, rem);

	const uint32_t e = _dagSize128 - rem;

	// This power works if e < 2**leadingZeros.
	if (e < (1U << leadingZeros))
	{
		shift2 = leadingZeros;
		_flags += " -D ALGO_STEPS=1 ";
	}
	else
	{
		magic += magic;
		const uint32_t twice_rem = rem + rem;
		if (twice_rem >= _dagSize128 || twice_rem < rem) 
			magic += 1;
		shift2 = leadingZeros;
		_flags += " -D ALGO_STEPS=2 ";
	}
	_flags += " -D SHIFT2=" + to_string(shift2) + " -D MAGIC=" + to_string(magic + 1);
}


bool assembleBinary(int _groupSize, int _dagSize128, cl::Device _device, int _gpu, std::string &_outfile)
{
	string assembler = ProgOpt::Get("Kernel", "CLRXAssembler");
	if (!fileExists(assembler))
	{
		LogB << "Unable to locate CLRX Assembler. Looking for \"" << assembler << "\"";
		return false;
	}
	string folder = ProgOpt::Get("Kernel", "SrcFolder");
	string source = ProgOpt::Get("Kernel", "SrcFile");

	ostringstream ss;
	ss << "ethash" << _gpu << ".out";
	_outfile = ss.str();

	string flags = " --64bit -D GROUP_SIZE=" + to_string(_groupSize) + " -D DAG_SIZE=" + to_string(_dagSize128) + " ";
	DivMagic(_dagSize128, flags);
	// just a little trick to get rid of the null at the end, which always seems to be there.
	string device = string(_device.getInfo<CL_DEVICE_NAME>().c_str());
	flags += " --gpuType=" + device;
	flags += " -I " + quote(folder) + " -o " + quote(folder + "/" + _outfile);
	string cmdline = quote(assembler) + " " + flags + " " + quote(folder + "/" + source);
	cmdline = quote(cmdline);
	bool result = 0 == system(cmdline.c_str());
	this_thread::sleep_for(chrono::milliseconds(50));
	return result;
}

bool loadProgramBinary(const char *filename, char* &p, int &len)
{
	size_t size;
	std::fstream f(filename, (std::fstream::in | std::fstream::binary));
	if (!f.is_open())
	{
		return false;
	}
	else
	{
		size_t fileSize;
		f.seekg(0, std::fstream::end);
		size = fileSize = (size_t) f.tellg();
		f.seekg(0, std::fstream::beg);

		p = new char[size + 1];
		if (!p)
		{
			f.close();
			return false;
		}

		f.read(p, fileSize);
		f.close();
		len = (int) size;
		return true;
	}
}

bool ethash_cl_miner::buildBinary(cl::Device& _device, std::string &_outfile)
{
	cl::Program::Binaries binaries;
	char* p;
	int len;
	string binaryFile = ProgOpt::Get("Kernel", "SrcFolder") + "/" + _outfile;
	if (!loadProgramBinary(binaryFile.c_str(), p, len))
	{
		LogB << "Failed loading kernel binary : \"" << binaryFile << "\"";
		return false;
	}
	binaries.push_back({p, len});
	m_programBin = cl::Program(m_context, {_device}, binaries);
	bool built = false;
	try
	{
		m_programBin.build({_device});
		built = true;
		m_searchKernel = cl::Kernel(m_programBin, "ethash_search");
	}
	catch (cl::Error const& err)
	{
		if (!built)
			LogB << " Build error : " << err.err() << " \n" << m_programBin.getBuildInfo<CL_PROGRAM_BUILD_LOG>(_device) << "\n";
		else
			LogB << " Kernel error : " << err.err() << " Did you specify an existing kernel name? " << err.what() << "\n";
		return false;
	}

	return true;
}


uint64_t ethash_cl_miner::nextNonceIndex(uint64_t &_nonceIndex, bool _overrideRandom) {
	// even if we are using the linear method, we still need to generate a random nonce at the begining
	// of each work package, which is what the override parameter is for.
	if (m_nonceGeneration == c_nonceLinear && !_overrideRandom)
		return _nonceIndex++;
	else {
		// look for a new random nonce index we (or any other miner) haven't checked yet.
		do {
			_nonceIndex = m_randDist(m_randGen);
		} while (!m_owner->storeNonceIndex(_nonceIndex));
		return _nonceIndex;
	}
}


bool ethash_cl_miner::init(unsigned _platformId, unsigned _deviceId)
{
	// get all platforms
	try
	{
		// this mutex is an attempt to solve some crashes I was seeing on the call to cl::CommandQueue() below.
		// OpenCL calls are supposed to be thread safe, but this seems to help.
		static Mutex x_init;
		UniqueGuard l(x_init);
		LogF << "Trace: ethash_cl_miner::init-1, device[" << _deviceId << "]";
		vector<cl::Platform> platforms = getPlatforms();
		if (platforms.empty())
			return false;
		
		// use selected platform
		_platformId = min<unsigned>(_platformId, platforms.size() - 1);

		// just a little trick to get rid of the null at the end, which always seems to be there.
		string platformName = string(platforms[_platformId].getInfo<CL_PLATFORM_NAME>().c_str());

		int platformId = OPENCL_PLATFORM_UNKNOWN;

		if (platformName == "NVIDIA CUDA")
		{
			platformId = OPENCL_PLATFORM_NVIDIA;
			LogS << "Using platform: " << platformName.c_str();
		}
		else if (platformName == "AMD Accelerated Parallel Processing")
		{
			platformId = OPENCL_PLATFORM_AMD;
			LogS << "Using platform: " << platformName.c_str();
		}
		else
		{
			LogS << "\nUnknown platform!!! (" << platformName.c_str() << ").  Using generic (non-optimized) algorithms.\n";
		}

		// get GPU device of the default platform
		vector<cl::Device> devices = getDevices(platforms, _platformId);
		if (devices.empty())
		{
			LogS << "No OpenCL devices found.";
			return false;
		}

		// use selected device
		LogF << "Trace: ethash_cl_miner::init-2, device[" << _deviceId << "]";
		m_device = _deviceId;
		cl::Device& cl_device = devices[min<unsigned>(_deviceId, devices.size() - 1)];
		string device_version = cl_device.getInfo<CL_DEVICE_VERSION>();
		LogB << "Using device: " << cl_device.getInfo<CL_DEVICE_NAME>().c_str() 
				  << ", version = " << device_version.c_str() << ", deviceID = " << _deviceId;

		if (strncmp("OpenCL 1.0", device_version.c_str(), 10) == 0)
		{
			LogS << "OpenCL 1.0 is not supported.";
			return false;
		}
		if (strncmp("OpenCL 1.1", device_version.c_str(), 10) == 0)
			m_openclOnePointOne = true;

		LogF << "Trace: ethash_cl_miner::init-3, device[" << _deviceId << "]";
		char options[256];
		int computeCapability = 0;
		if (platformId == OPENCL_PLATFORM_NVIDIA) {
			cl_uint computeCapabilityMajor;
			cl_uint computeCapabilityMinor;
			clGetDeviceInfo(cl_device(), CL_DEVICE_COMPUTE_CAPABILITY_MAJOR_NV, sizeof(cl_uint), &computeCapabilityMajor, NULL);
			clGetDeviceInfo(cl_device(), CL_DEVICE_COMPUTE_CAPABILITY_MINOR_NV, sizeof(cl_uint), &computeCapabilityMinor, NULL);

			computeCapability = computeCapabilityMajor * 10 + computeCapabilityMinor;
			int maxregs = computeCapability >= 35 ? 72 : 63;
			sprintf(options, "-cl-nv-maxrregcount=%d", maxregs);// , computeCapability);
		}
		else {
			sprintf(options, "%s", "");
		}
		// create context
		m_context = cl::Context(vector<cl::Device>(&cl_device, &cl_device + 1));
		LogF << "Trace: ethash_cl_miner::init-3a, device[" << _deviceId << "]";
		for (int i = 0; i < c_bufferCount; i++)
			m_queue[i] = cl::CommandQueue(m_context, cl_device);

		l.unlock();

		// make sure that global work size is evenly divisible by the local workgroup size
		LogF << "Trace: ethash_cl_miner::init-4, device[" << _deviceId << "]";
		m_globalWorkSize = s_initialGlobalWorkSize;
		if (m_globalWorkSize % s_workgroupSize != 0)
			m_globalWorkSize = ((m_globalWorkSize / s_workgroupSize) + 1) * s_workgroupSize;

		// patch source code
		// note: ETHASH_CL_MINER_KERNEL is simply ethash_cl_miner_kernel.cl compiled
		// into a byte array by bin2h.cmake. There is no need to load the file by hand in runtime
		string code(ETHASH_CL_MINER_KERNEL, ETHASH_CL_MINER_KERNEL + ETHASH_CL_MINER_KERNEL_SIZE);
		addDefinition(code, "GROUP_SIZE", s_workgroupSize);
		addDefinition(code, "ACCESSES", ETHASH_ACCESSES);
		addDefinition(code, "MAX_OUTPUTS", c_maxSearchResults);
		addDefinition(code, "PLATFORM", platformId);
		addDefinition(code, "COMPUTE", computeCapability);

		// create miner OpenCL program
		cl::Program::Sources sources;
		sources.push_back({code.c_str(), code.size()});
		m_programCL = cl::Program(m_context, sources);
		bool built = false;
		try
		{
			m_programCL.build({cl_device}, options);
			built = true;
			m_searchKernel = cl::Kernel(m_programCL, "bitcoin0x_search");
		}
		catch (cl::Error const& err)
		{
			if (!built)
				LogB << " Build error : " << err.err() << " \n" << m_programCL.getBuildInfo<CL_PROGRAM_BUILD_LOG>(cl_device) << "\n";
			else
				LogB << " Kernel error : " << err.err() << " Did you specify an existing kernel name? " << err.what() << "\n";
			return false;
		}
		
		// buffers
		
		for (unsigned i = 0; i != c_bufferCount; ++i)
		{
			m_searchBuffer[i] = cl::Buffer(m_context, CL_MEM_READ_WRITE, sizeof(search_results));
			// first element of solutions will store the number of hash solutions found
			m_queue[0].enqueueWriteBuffer(m_searchBuffer[i], false, 0, 4, &c_zero);
			m_precompBuffer[i] = cl::Buffer(m_context, CL_MEM_READ_ONLY, 200);
		}

	}
	catch (cl::Error const& err)
	{
		LogB << err.what() << "(" << err.err() << ")";
		return false;
	}
	LogF << "Trace: ethash_cl_miner::init-exit, device[" << _deviceId << "]";
	return true;
}


void ethash_cl_miner::search(bytes _challenge, uint64_t _target, h160 _miningAccount, search_hook& _hook)
{
	try
	{
		LogF << "Trace: ethash_cl_miner::search-1, challenge = " << toHex(_challenge).substr(0, 8) << ", target = "
			<< std::hex << std::setw(16) << std::setfill('0') << _target << ", miningAccount = " << _miningAccount.hex() 
			<< ", device[" << m_device << "]";

		int l_throttle = 0;		// percent throttling
		int l_bufferCount;
		// used for throttling calculations. does not include throttling delays.
		Timer kernelTimer;
		// put in a rough guess for now, in case we are throttling
		int kernelTime = 100;
		int batchCount = 0;

		{
			// if we're throttling we only use one buffer to keep things linear, so we can do a 
			// pause inbetween each kernel run.
			ReadGuard l(x_throttle);
			l_bufferCount = (m_throttle == 0) ? c_bufferCount : 1;
		}

		h256 nonce;
		uint8_t message[88] = {0};
		memcpy(&message[0], _challenge.data(), 32);
		memcpy(&message[32], _miningAccount.data(), 20);

		m_searchKernel.setArg(2, _target);

		LogF << "Trace: ethash_cl_miner::search-2, device[" << m_device << "]";

		while (true)
		{
			// throttling: when we transition from not throttling to throttling, we skip the step
			// of enqueueing a kernel run, on a one-time basis, and instead wait for the current
			// kernel to finish. that way we can put a delay inbetween every kernel run.
			// when we transition from throttling to not throttling, we skip the step of waiting
			// for the kernel to finish and reading the results, and instead immediately queue up another kernel run.

			checkThrottleChange(l_throttle, l_bufferCount);
			if (l_throttle > 0)
			{
				int millisDelay;
				if (l_throttle < 100)
				{
					millisDelay = int(l_throttle * kernelTime / (100.0 - l_throttle));
					LogF << "Throttle: Sleeping for " << millisDelay << " ms, device[" << m_device << "]";
				}
				else
				{
					millisDelay = 100;
					m_pending.clear();
					LogF << "Throttle: Sleeping indefinitely : 100% throttle, device[" << m_device << "]";
				}

				while (millisDelay > 0)
				{
					int thisDelay = min(100, millisDelay);
					this_thread::sleep_for(chrono::milliseconds(thisDelay));
					millisDelay = (l_throttle == 100) ? 100 : millisDelay - thisDelay;
					checkThrottleChange(l_throttle, l_bufferCount);
					if (l_throttle == 100) 
						_hook.searched(0, 0, c_maxHash);	// keep the hash rates display up-to-date
					// check for new work package
					if (_hook.shouldStop())
					{
						LogF << "Throttle: Sleeping interrupted by new work package, device[" << m_device << "]";
						// I'd use break but we're two levels deep.
						goto out;
					}
				}
			}

			// queue up a kernel run. 
			// old strategy : the idea is to always have one kernel running, and another queued up to run
			// right after. that way we can process the results of the first kernel without holding up the next
			// kernel.  the original ethminer program tried to do it with only one queue, two 'results' buffers and
			// a blocking call to enqueueMapBuffer, which on paper seems like it would to the trick, but it wasn't.
			// what would actually happen was that the enqueueMapBuffer call would block until BOTH kernels had finished.
			// (You can use CodeXL to do a timeline trace to see for yourself).  What really needs to be done is
			// have two command queues, two results buffers, and a non-blocking call to enqueueMapBuffer (with associated
			// events to know when the data is available). this causes both kernels to run at the same time.  

			kernelTimer.restart();
			if (m_pending.size() < l_bufferCount)
			{
				nonce = h256::random();
				memcpy(&message[52], nonce.data(), 32);

				uint64_t precomp[25];
				keccak_precomp(precomp, (uint64_t*) message);
				m_queue[m_buf].enqueueWriteBuffer(m_precompBuffer[m_buf], CL_TRUE, 0, 200, precomp);

				m_searchKernel.setArg(0, m_precompBuffer[m_buf]);
				m_searchKernel.setArg(1, m_searchBuffer[m_buf]);

				m_queue[m_buf].enqueueNDRangeKernel(m_searchKernel, cl::NullRange, m_globalWorkSize, s_workgroupSize);
				m_pending.push_back({nonce, m_buf});

				m_results[m_buf] = (search_results*) m_queue[m_buf].enqueueMapBuffer(m_searchBuffer[m_buf], CL_FALSE, CL_MAP_READ, 0, 
																			   sizeof(search_results), 0, &m_mapEvents[m_buf]);
				m_buf = (m_buf + 1) % l_bufferCount;
			}

			if (m_pending.size() == l_bufferCount)
			{
				// read results

				pending_batch batch = m_pending.front();
				m_pending.pop_front();

				// this blocks until the kernel finishes
				m_mapEvents[batch.buf].wait();

				kernelTime = kernelTimer.elapsedMilliseconds();

				unsigned num_found = min<unsigned>(m_results[batch.buf]->solutions[0], c_maxSearchResults);
				h256 nonces[c_maxSearchResults];
				for (unsigned i = 0; i != num_found; ++i) {

					// in the kernel, the work item number is written into state[8], prior to doing the keccak hash, so we need 
					// to do the same thing here so we can check the result.  that means writing the 
					// solution starting at byte 12 of the nonce.

					uint64_t soln = m_results[batch.buf]->solutions[i + 1];
					nonces[i] = batch.nonce;
					uint8_t* x = (uint8_t*) nonces[i].data();
					*(uint64_t*) (&x[12]) = soln;
				}

				m_queue[batch.buf].enqueueUnmapMemObject(m_searchBuffer[batch.buf], m_results[batch.buf]);

				if (num_found) {
					m_queue[batch.buf].enqueueWriteBuffer(m_searchBuffer[batch.buf], false, 0, 4, &c_zero);
					if (_hook.found(nonces, num_found))
						break;
				}
				if (batch.buf == 0)
					// hash rates come out smoother if we report regularly.
					m_owner->accumulateHashes(m_globalWorkSize * l_bufferCount, batchCount++);
				if (_hook.searched(0, 0, m_bestHash))
					break;
			}
		}

	out: ;


	}
	catch (cl::Error const& err)
	{
		LogB << err.what() << "(" << err.err() << ")";
	}
	LogF << "Trace: ethash_cl_miner::search-exit, device[" << m_device << "]";
}

void ethash_cl_miner::checkThrottleChange(int& _localThrottle, int& _bufferCount)
{
	ReadGuard l(x_throttle);
	if (m_throttle > 0 && _localThrottle == 0)
	{
		_bufferCount = 1;
		LogF << "Throttle: Start throttling, device[" << m_device << "]";
	}
	else if (_localThrottle > 0 && m_throttle == 0)
	{
		_bufferCount = c_bufferCount;
		LogF << "Throttle: Stop throttling, device[" << m_device << "]";
	}
	_localThrottle = m_throttle;
}

void ethash_cl_miner::setThrottle(int _percent)
{
	WriteGuard l(x_throttle);
	m_throttle = _percent;
}
