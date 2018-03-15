
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

#include <string>
#include <vector>
#include <stdint.h>
#include <json/json.h>

using namespace std;


class DataLogger
{

public:

	DataLogger();
	void writeToDisk();
	void recordBestHash(uint64_t _bh);
	void recordCloseHit(uint64_t _closeHit, unsigned _work, int _gpuMiner);
	void recordHashFault(int _gpuMiner);
	void recordSolution(unsigned _blockNumber, int _state, bool _stale, int _gpuMiner);
	int solutionCount();
	int closeHitCount();
	int hashFaultCount();
	uint64_t retrieveBestHash(void);
	std::string retrieveBestHashDate(void);
	Json::Value retrieveCloseHits(bool _clear);
	Json::Value retrieveHashFaults(bool _clear);
	std::string retrieveLastSolution(void);
	Json::Value retrieveSolutions(bool _clear);
	std::string now();

	void test();

private:
	Json::Value miningResults;
	std::string logfilePath(void);

};

