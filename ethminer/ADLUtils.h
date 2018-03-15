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


// get GPU temperature data using the AMD ADL SDK

#include <ethminer/MultiLog.h>
#include <vector>

#if defined (WIN32)
#include <windows.h>
#include <tchar.h>
#include <amd_adl/adl_sdk.h> 
#else
#include "../extdep/include/amd_adl/adl_sdk.h"
//#include "../../include/customer/oem_structures.h"
#include <dlfcn.h>	//dyopen, dlsym, dlclose
#include <stdlib.h>	
#include <string.h>	//memeset
#include <unistd.h>	//sleep
#endif

#include <stdio.h>
#include <libdevcore/Guards.h>

using namespace dev;

#define AMDVENDORID             (1002)
#define ADL_WARNING_NO_DATA      -100


class ADLUtils
{
public:
	// retrieve temperature for indicated gpu
	double getTemps(int _gpu);
	int getFanSpeed(int _gpu);

private:
	void init();
	int getAdapterIndex(int _gpu);
	// we're just going to support overdrive 6 and greater.  it has been out since at least 2014, maybe earlier.
	double getTemps_OD6(int _gpu, int _arrayIndex);
	double getTemps_ODN(int _gpu, int _arrayIndex);
	int getFanSpeed_OD6(int _gpu, int _arrayIndex);
	int getFanSpeed_ODN(int _gpu, int _arrayIndex);
private:

	typedef int(*ADL2_MAIN_CONTROL_CREATE)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE*);
	typedef int(*ADL2_ADAPTER_NUMBEROFADAPTERS_GET) (ADL_CONTEXT_HANDLE, int*);
	typedef int(*ADL2_ADAPTER_ADAPTERINFO_GET) (ADL_CONTEXT_HANDLE, LPAdapterInfo, int);
	typedef int(*ADL2_ADAPTER_ACTIVE_GET) (ADL_CONTEXT_HANDLE, int, int*);
	typedef int(*ADL2_OVERDRIVE_CAPS) (ADL_CONTEXT_HANDLE, int iAdapterIndex, int *iSupported, int *iEnabled, int *iVersion);
	typedef int(*ADL2_OVERDRIVE6_THERMALCONTROLLER_CAPS)(ADL_CONTEXT_HANDLE, int iAdapterIndex, ADLOD6ThermalControllerCaps *lpThermalControllerCaps);
	typedef int(*ADL2_OVERDRIVE6_TEMPERATURE_GET)(ADL_CONTEXT_HANDLE, int iAdapterIndex, int *lpTemperature);
	typedef int(*ADL2_OVERDRIVE6_FANSPEED_GET)(ADL_CONTEXT_HANDLE, int iAdapterIndex, ADLOD6FanSpeedInfo *lpFanSpeedInfo);
	typedef int(*ADL2_OVERDRIVEN_TEMPERATURE_GET) (ADL_CONTEXT_HANDLE, int, int, int*);
	typedef int(*ADL2_OVERDRIVEN_FANCONTROL_GET) (ADL_CONTEXT_HANDLE, int, ADLODNFanControl*);
	typedef int(*ADL2_OVERDRIVEN_FANCONTROL_SET) (ADL_CONTEXT_HANDLE, int, ADLODNFanControl*);

	ADL2_MAIN_CONTROL_CREATE					ADL2_Main_Control_Create;
	ADL2_ADAPTER_NUMBEROFADAPTERS_GET			ADL2_Adapter_NumberOfAdapters_Get;
	ADL2_ADAPTER_ADAPTERINFO_GET				ADL2_Adapter_AdapterInfo_Get;
	ADL2_ADAPTER_ACTIVE_GET						ADL2_Adapter_Active_Get;
	ADL2_OVERDRIVE_CAPS							ADL2_Overdrive_Caps;
	ADL2_OVERDRIVE6_THERMALCONTROLLER_CAPS		ADL2_Overdrive6_ThermalController_Caps;
	ADL2_OVERDRIVE6_TEMPERATURE_GET				ADL2_Overdrive6_Temperature_Get;
	ADL2_OVERDRIVE6_FANSPEED_GET				ADL2_Overdrive6_FanSpeed_Get;
	ADL2_OVERDRIVEN_TEMPERATURE_GET				ADL2_OverdriveN_Temperature_Get = NULL;
	ADL2_OVERDRIVEN_FANCONTROL_GET				ADL2_OverdriveN_FanControl_Get = NULL;
	ADL2_OVERDRIVEN_FANCONTROL_SET				ADL2_OverdriveN_FanControl_Set = NULL;

	LPAdapterInfo								lpAdapterInfo = NULL;

	#if defined (WIN32)
	HINSTANCE hDLL;		// Handle to DLL
	#else
	void *hDLL;			// Handle to .so library
	#endif

	bool initialized = false;
	ADL_CONTEXT_HANDLE context = NULL; 
	int  iNumberAdapters = 0;
	int  iOverdriveSupported = 0;
	int  iOverdriveEnabled = 0;
	int	 iOverdriveVersion = 0;
	std::map<int, int> adapterIndices;
	mutable Mutex x_temps;

};


extern ADLUtils g_ADLUtils;