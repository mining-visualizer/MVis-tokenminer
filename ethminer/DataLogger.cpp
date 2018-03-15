
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

#include "DataLogger.h"
#include <string>
#include <fstream> 
#include <iostream> 
#include <sstream>
#include <iomanip>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "Misc.h"
#include "MultiLog.h"


using namespace boost;


/*-----------------------------------------------------------------------------------
* constructor
*----------------------------------------------------------------------------------*/
DataLogger::DataLogger()
{
	ifstream f;

	f.exceptions(ofstream::failbit | ofstream::badbit);
	string path = logfilePath();
	try
	{
		Json::CharReaderBuilder rbuilder;
		if (filesystem::exists(path))
		{
			f.open(path, fstream::in);
			std::string errs;
			bool ok = Json::parseFromStream(rbuilder, f, &miningResults, &errs);
			if (!ok)
				throw std::runtime_error(errs.c_str());
			f.exceptions(ofstream::goodbit);
			f.close();
		}
	}
	catch (std::exception& e)
	{
		LogB << "Error reading \"" << path << "\"";
		LogB << "Message : " << e.what();
		LogB << "Mining results reset to empty state.";
		f.exceptions(ofstream::goodbit);
		f.close();
		miningResults = Json::Value(Json::objectValue);
		writeToDisk();
	}


}	// constructor


/*-----------------------------------------------------------------------------------
* writeToDisk
*----------------------------------------------------------------------------------*/
void DataLogger::writeToDisk()
{

	ofstream f;

	f.exceptions(ofstream::failbit | ofstream::badbit);
	string path = logfilePath();

	try
	{
		Json::StreamWriterBuilder wbuilder;
		wbuilder["indentation"] = "    ";
		std::string document = Json::writeString(wbuilder, miningResults);

		f.open(path, fstream::trunc);
		f << document << std::endl << std::flush;
		f.close();
	}
	catch (std::exception& e)
	{
		LogB << "Error writing \"" << path << "\"";
		LogB << "Message : " << e.what();
	}

}	// write


/*-----------------------------------------------------------------------------------
* logfilePath
*----------------------------------------------------------------------------------*/
std::string DataLogger::logfilePath(void)
{
	filesystem::path path = getAppDataFolder();
	path = path / "mining_data.json";
	return path.generic_string();
}	// logfilePath


/*-----------------------------------------------------------------------------------
* now
*----------------------------------------------------------------------------------*/
string DataLogger::now()
{
	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);

	std::ostringstream oss;
	oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
	return oss.str();
}	// now


/*-----------------------------------------------------------------------------------
* recordBestHash
*----------------------------------------------------------------------------------*/
void DataLogger::recordBestHash(uint64_t _bh)
{
	miningResults["BestHash"] = (Json::UInt64)_bh;
	miningResults["BestHashDate"] = now();
	writeToDisk();
}	// recordBestHash


/*-----------------------------------------------------------------------------------
* recordCloseHit
*----------------------------------------------------------------------------------*/
void DataLogger::recordCloseHit(uint64_t _closeHit, unsigned _work, int _gpuMiner)
{
	Json::Value closeHit;
	closeHit["date"] = now();
	closeHit["close_hit"] = (Json::UInt64)_closeHit;
	closeHit["work"] = _work;
	closeHit["gpu_miner"] = _gpuMiner;
	miningResults["CloseHits"].append(closeHit);
	writeToDisk();
}


/*-----------------------------------------------------------------------------------
* recordHashFault
*----------------------------------------------------------------------------------*/
void DataLogger::recordHashFault(int _gpuMiner)
{
	Json::Value hashFault;
	hashFault["date"] = now();
	hashFault["gpu_miner"] = _gpuMiner;
	miningResults["HashFaults"].append(hashFault);
	writeToDisk();
}


/*-----------------------------------------------------------------------------------
* recordSolution
*----------------------------------------------------------------------------------*/
void DataLogger::recordSolution(unsigned _blockNumber, int _state, bool _stale, int _gpuMiner)
{
	Json::Value solution;
	solution["date"] = now();
	solution["block"] = _blockNumber;
	solution["state"] = _state;
	solution["stale"] = _stale;
	solution["gpu_miner"] = _gpuMiner;
	miningResults["Solutions"].append(solution);
	writeToDisk();

}	// recordSolution


/*-----------------------------------------------------------------------------------
* solutionCount
*----------------------------------------------------------------------------------*/
int DataLogger::solutionCount()
{
	return miningResults["Solutions"].size();

}	// solutionCount


/*-----------------------------------------------------------------------------------
* closeHitCount
*----------------------------------------------------------------------------------*/
int DataLogger::closeHitCount()
{
	return miningResults["CloseHits"].size();

}	// closeHitCount


/*-----------------------------------------------------------------------------------
* hashFaultCount
*----------------------------------------------------------------------------------*/
int DataLogger::hashFaultCount()
{
	return miningResults["HashFaults"].size();

}	// hashFaultCount


/*-----------------------------------------------------------------------------------
* retrieveBestHash
*----------------------------------------------------------------------------------*/
uint64_t DataLogger::retrieveBestHash(void)
{
	return miningResults.get("BestHash", (Json::UInt64)~uint64_t(0)).asUInt64();

}	// retrieveBestHash


/*-----------------------------------------------------------------------------------
* retrieveBestHashDate
*----------------------------------------------------------------------------------*/
std::string DataLogger::retrieveBestHashDate(void)
{
	return miningResults.get("BestHashDate", "").asString();

}	// retrieveBestHashDate


/*-----------------------------------------------------------------------------------
* retrieveCloseHits
*----------------------------------------------------------------------------------*/
Json::Value DataLogger::retrieveCloseHits(bool _clear)
{
	Json::Value closeHits = miningResults["CloseHits"];
	if (_clear)
	{
		miningResults.removeMember("CloseHits");
		writeToDisk();
	}
	return closeHits;

}	// retrieveCloseHits


/*-----------------------------------------------------------------------------------
* retrieveHashFaults
*----------------------------------------------------------------------------------*/
Json::Value DataLogger::retrieveHashFaults(bool _clear)
{
	Json::Value hashFaults = miningResults["HashFaults"];
	if (_clear)
	{
		miningResults.removeMember("HashFaults");
		writeToDisk();
	}
	return hashFaults;

}	// retrieveHashFaults


/*-----------------------------------------------------------------------------------
* retrieveLastSolution
*----------------------------------------------------------------------------------*/
std::string DataLogger::retrieveLastSolution(void)
{
	Json::Value solutions = miningResults["Solutions"];
	if (solutions.size() > 0)
	{
		return solutions[solutions.size() - 1]["date"].asString();
	} 
	else
		return "";

}	// retrieveLastSolution


/*-----------------------------------------------------------------------------------
* retrieveSolutions
*----------------------------------------------------------------------------------*/
Json::Value DataLogger::retrieveSolutions(bool _clear)
{
	Json::Value solutions = miningResults["Solutions"];
	if (_clear)
	{
		miningResults.removeMember("Solutions");
		writeToDisk();
	}
	return solutions;

}	// retrieveSolutions


/*-----------------------------------------------------------------------------------
* test
*----------------------------------------------------------------------------------*/
void DataLogger::test()
{
	LogS << "DataLogger::test > " << miningResults["BestHash"].size() << ", " << miningResults["asdf"].size() << ", " << miningResults["Solutions"].size();



}

