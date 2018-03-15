

/*
* SpeedFan Information Tool 1.0
*
* Retrieves temperature information from SpeedFan and outputs it to the console.
*
* (c) 2008, Christopher Vagnetoft
* Free to use and reuse under the GNU Public License (GPL) v2.
*/

#include <vector>
#include <windows.h>
#include <libdevcore/Guards.h>

using namespace dev;

class SpeedFan
{

public:

	enum DataClass
	{
		Temperatures,
		FanSpeeds,
		Voltages
	};

	void getData(std::vector<double>& _data, DataClass _class, int _count);

private:

	// pragma pack is included here because the struct is a pascal Packed Record,
	// meaning that fields aren't aligned on a 4-byte boundary. 4 bytes fit 2
	// 2-byte records.
	#pragma pack(push, 1)

	// This is the struct we're using to access the shared memory.
	struct SFMemory
	{
		WORD version;
		WORD flags;
		int MemSize;
		int handle;
		WORD NumTemps;
		WORD NumFans;
		WORD NumVolts;
		signed int temps[32];
		signed int fans[32];
		signed int volts[32];
	};

	#pragma pack(pop)

	// Name of filename and memory map name to open.
	const char* filename = "SFSharedMemory_ALM";
	const char* mapname = "SFSharedMemory_ALM";

	UINT nSize = sizeof(SFMemory);
	mutable Mutex x_temps;

};

extern SpeedFan g_SpeedFan;
