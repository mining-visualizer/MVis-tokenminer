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
/** @file EthashCPUMiner.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 *
 * Determines the PoW algorithm.
 */

#include "EthashCPUMiner.h"
#include <thread>
#include <chrono>
#include <boost/algorithm/string.hpp>
#include <random>
#include <libethash/sha3_cryptopp.h>
#if ETH_CPUID || !ETH_TRUE
#define HAVE_STDINT_H
#include <libcpuid/libcpuid.h>
#endif

using namespace std;
using namespace dev;
using namespace eth;

unsigned EthashCPUMiner::s_numInstances = 0;

#if ETH_CPUID || !ETH_TRUE
static string jsonEncode(map<string, string> const& _m)
{
	string ret = "{";

	for (auto const& i: _m)
	{
		string k = boost::replace_all_copy(boost::replace_all_copy(i.first, "\\", "\\\\"), "'", "\\'");
		string v = boost::replace_all_copy(boost::replace_all_copy(i.second, "\\", "\\\\"), "'", "\\'");
		if (ret.size() > 1)
			ret += ", ";
		ret += "\"" + k + "\":\"" + v + "\"";
	}

	return ret + "}";
}
#endif

EthashCPUMiner::EthashCPUMiner(Farm* _farm, unsigned _index):
	GenericMiner<EthashProofOfWork>(_farm, _index), Worker("miner" + toString(index()))
{
}

EthashCPUMiner::~EthashCPUMiner()
{
}

void EthashCPUMiner::kickOff()
{
	LogF << "Trace: EthashCPUMiner::kickOff, miner[" << m_index << "]";
	startWorking();
}

void EthashCPUMiner::pause()
{
	LogF << "Trace: EthashCPUMiner::pause, miner[" << m_index << "]";
	stopWorking();
}

void EthashCPUMiner::workLoop() {
	LogF << "Trace: EthashCPUMiner::workLoop, miner[" << m_index << "]";

	Timer batchTime;
	unsigned hashCount = 1;
	uint64_t batchCount = 0;
	h256 nonce = h256::random();
	h256 hash;
	h160 sender(m_farm->hashingAcct);
	bytes mix(84);
	memcpy(&mix[0], challenge.data(), 32);
	memcpy(&mix[32], sender.data(), 20);
	memcpy(&mix[52], nonce.data(), 32);
	h256* noncePtr = (h256*) &mix[52];

	m_farm->setIsMining(true);

	for (; !shouldStop(); hashCount++, ++(*noncePtr)) {
		SHA3_256((const ethash_h256_t*) &hash, (const uint8_t*) mix.data(), 84);
		if (hash < target && submitProof(*noncePtr))
			break;

		if (batchTime.elapsedMilliseconds() > 100) {
			accumulateHashes(hashCount, batchCount++);
			batchTime.restart();
			hashCount = 1;
		}
	}
}

std::string EthashCPUMiner::platformInfo()
{
	string baseline = toString(std::thread::hardware_concurrency()) + "-thread CPU";
#if ETH_CPUID || !ETH_TRUE
	if (!cpuid_present())
		return baseline;
	struct cpu_raw_data_t raw;
	struct cpu_id_t data;
	if (cpuid_get_raw_data(&raw) < 0)
		return baseline;
	if (cpu_identify(&raw, &data) < 0)
		return baseline;
	map<string, string> m;
	m["vendor"] = data.vendor_str;
	m["codename"] = data.cpu_codename;
	m["brand"] = data.brand_str;
	m["L1 cache"] = toString(data.l1_data_cache);
	m["L2 cache"] = toString(data.l2_cache);
	m["L3 cache"] = toString(data.l3_cache);
	m["cores"] = toString(data.num_cores);
	m["threads"] = toString(data.num_logical_cpus);
	m["clocknominal"] = toString(cpu_clock_by_os());
	m["clocktested"] = toString(cpu_clock_measure(200, 0));
	/*
	printf("  MMX         : %s\n", data.flags[CPU_FEATURE_MMX] ? "present" : "absent");
	printf("  MMX-extended: %s\n", data.flags[CPU_FEATURE_MMXEXT] ? "present" : "absent");
	printf("  SSE         : %s\n", data.flags[CPU_FEATURE_SSE] ? "present" : "absent");
	printf("  SSE2        : %s\n", data.flags[CPU_FEATURE_SSE2] ? "present" : "absent");
	printf("  3DNow!      : %s\n", data.flags[CPU_FEATURE_3DNOW] ? "present" : "absent");
	*/
	return jsonEncode(m);
#else
	return baseline;
#endif
}
