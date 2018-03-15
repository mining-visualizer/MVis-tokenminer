

/*
* SpeedFan Information Tool 1.0
*
* Retrieves temperature information from SpeedFan and outputs it to the console.
*
* (c) 2008, Christopher Vagnetoft
* Free to use and reuse under the GNU Public License (GPL) v2.
*/

#include "speedfan.h"
#include <algorithm>

SpeedFan g_SpeedFan;

void SpeedFan::getData(std::vector<double>& _data, DataClass _class, int _count)
{
	Guard l(x_temps);

	// speedfan gives temperatures multiplied by 100
	_data.clear();
	HANDLE file = (HANDLE) CreateFile(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		throw std::exception("Speedfan.getData : CreateFile failed with error ", GetLastError());
	HANDLE filemap = (HANDLE) CreateFileMapping(file, NULL, PAGE_READWRITE, 0, nSize, mapname);
	if (filemap == NULL)
		throw std::exception("Speedfan.getData : CreateFileMapping failed with error ", GetLastError());
	SFMemory* sfmemory = (SFMemory*) MapViewOfFile(filemap, FILE_MAP_READ, 0, 0, nSize);
	if (sfmemory == NULL)
		throw std::exception("Speedfan.getData : MapViewOfFile failed with error ", GetLastError());

	if (sfmemory)
	{
		int* pt;

		if (_class == Temperatures)
			pt = sfmemory->temps;
		else if (_class == FanSpeeds)
			pt = sfmemory->fans;
		else
			pt = sfmemory->volts;

		for (int i = 0; i < std::min(_count, 32); i++)
			_data.push_back(*(pt++) / 100.0);
	}
	CloseHandle(filemap);
	CloseHandle(file);
}

