
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

#include "ADLUtils.h"

ADLUtils g_ADLUtils;


// Memory allocation function
void* __stdcall ADL_Main_Memory_Alloc(int iSize)
{
	void* lpBuffer = malloc(iSize);
	return lpBuffer;
}

#if defined (LINUX)

void *GetProcAddress(void* pLibrary, const char* name)
{
	return dlsym(pLibrary, name);
}

#endif

void ADLUtils::init()
{
	LogF << "Trace: ADLUtils.init [in]";
	initialized = true;

#if defined (WIN32)
	hDLL = LoadLibrary("atiadlxx.dll");
	if (hDLL == NULL)
		// A 32 bit calling application on 64 bit OS will fail to LoadLIbrary.
		// Try to load the 32 bit library (atiadlxy.dll) instead
		hDLL = LoadLibrary("atiadlxy.dll");
#else
	hDLL = dlopen("libatiadlxx.so", RTLD_LAZY | RTLD_GLOBAL);
#endif

	if (NULL == hDLL)
	{
		LogD << "Exception: ADLUtils.init - failed to load atiadlxx.dll";
		return;
	}

	ADL2_Main_Control_Create = (ADL2_MAIN_CONTROL_CREATE) GetProcAddress(hDLL, "ADL2_Main_Control_Create");
	ADL2_Adapter_NumberOfAdapters_Get = (ADL2_ADAPTER_NUMBEROFADAPTERS_GET) GetProcAddress(hDLL, "ADL2_Adapter_NumberOfAdapters_Get");
	ADL2_Adapter_AdapterInfo_Get = (ADL2_ADAPTER_ADAPTERINFO_GET) GetProcAddress(hDLL, "ADL2_Adapter_AdapterInfo_Get");
	ADL2_Adapter_Active_Get = (ADL2_ADAPTER_ACTIVE_GET) GetProcAddress(hDLL, "ADL2_Adapter_Active_Get");
	ADL2_Overdrive_Caps = (ADL2_OVERDRIVE_CAPS) GetProcAddress(hDLL, "ADL2_Overdrive_Caps");
	ADL2_Overdrive6_ThermalController_Caps = (ADL2_OVERDRIVE6_THERMALCONTROLLER_CAPS) GetProcAddress(hDLL, "ADL2_Overdrive6_ThermalController_Caps");
	ADL2_Overdrive6_Temperature_Get = (ADL2_OVERDRIVE6_TEMPERATURE_GET) GetProcAddress(hDLL, "ADL2_Overdrive6_Temperature_Get");
	ADL2_Overdrive6_FanSpeed_Get = (ADL2_OVERDRIVE6_FANSPEED_GET) GetProcAddress(hDLL, "ADL2_Overdrive6_FanSpeed_Get");
	ADL2_OverdriveN_Temperature_Get = (ADL2_OVERDRIVEN_TEMPERATURE_GET) GetProcAddress(hDLL, "ADL2_OverdriveN_Temperature_Get");
	ADL2_OverdriveN_FanControl_Get = (ADL2_OVERDRIVEN_FANCONTROL_GET) GetProcAddress(hDLL, "ADL2_OverdriveN_FanControl_Get");
	ADL2_OverdriveN_FanControl_Set = (ADL2_OVERDRIVEN_FANCONTROL_SET) GetProcAddress(hDLL, "ADL2_OverdriveN_FanControl_Set");

	adapterIndices.clear();

	// Initialize ADL. The second parameter is 1, which means:
	// retrieve adapter information only for adapters that are physically present and enabled in the system
	if (ADL_OK != ADL2_Main_Control_Create(ADL_Main_Memory_Alloc, 1, &context))
	{
		LogB << "Exception: ADLUtils.init - ADL Initialization Error!";
		return;
	}

	// Obtain the number of adapters for the system
	ADL2_Adapter_NumberOfAdapters_Get(context, &iNumberAdapters);
	LogF << "ADLUtils.init - iNumberAdapters = " << iNumberAdapters;
	if (iNumberAdapters > 0)
	{
		lpAdapterInfo = (LPAdapterInfo) malloc(sizeof(AdapterInfo) * iNumberAdapters);
		memset(lpAdapterInfo, '\0', sizeof(AdapterInfo) * iNumberAdapters);

		// Get the AdapterInfo structure for all adapters in the system
		ADL2_Adapter_AdapterInfo_Get(context, lpAdapterInfo, sizeof(AdapterInfo) * iNumberAdapters);
	}

	// loop through available GUPs, build a mapping structure, and log any errors
	int busNumber = -1;
	int adapterCounter = -1;
	for (int i = 0; i < iNumberAdapters; i++)
	{
		AdapterInfo& adapter = lpAdapterInfo[i];
		LogF << "ADLUtils.init: iAdapterIndex = " << adapter.iAdapterIndex << ", strAdapterName = " << adapter.strAdapterName
			<< ", strDisplayName = " << adapter.strDisplayName << ", iDeviceNumber = " << adapter.iDeviceNumber << ", strUDID = "
			<< adapter.strUDID << ", iBusNumber = " << adapter.iBusNumber;

		if (adapter.iBusNumber < 0) continue;
		// check if the adapter is active
		//int adapterActive = 0;
		//ADL2_Adapter_Active_Get(context, adapter.iAdapterIndex, &adapterActive);
		//if (!adapterActive) continue;
		// this, I believe, will tell us if we're looking at the same gpu as the previous one.
		if (adapter.iBusNumber == busNumber) continue;

		// we've got a new adapter
		adapterCounter++;
		busNumber = adapter.iBusNumber;
		// assume failure to start with (ie. -1)
		adapterIndices.insert(std::pair<int, int>(adapterCounter, -1));

		if (adapter.iVendorID != AMDVENDORID)
			continue;

		if (ADL_OK != ADL2_Overdrive_Caps(context, adapter.iAdapterIndex, &iOverdriveSupported, &iOverdriveEnabled, &iOverdriveVersion))
		{
			LogB << "Exception: ADLUtils.init - Can’t get Overdrive capabilities on GPU[" << adapterCounter << "]";
			continue;
		}
		if (!iOverdriveSupported)
		{
			LogB << "Exception: ADLUtils.init - Overdrive is not supported on GPU[" << adapterCounter << "]";
			continue;
		}
		else if (iOverdriveVersion < 6)
		{
			LogB << "Exception: ADLUtils.init - Overdrive version 6 or greater is not supported by GPU[" << adapterCounter << "]";
			continue;
		}
		adapterIndices[adapterCounter] = i;
	}
	LogF << "Trace: ADLUtils.init [out]";
}	// init()

int ADLUtils::getAdapterIndex(int _gpu)
{
	if (!initialized)
		init();

	if (adapterIndices.find(_gpu) == adapterIndices.end())
		return -1;
	else
		return adapterIndices[_gpu];
}

// retrieve temperature for indicated gpu.  
double ADLUtils::getTemps(int _gpu)
{
	Guard l(x_temps);
	double res = 0.0;

	int arrayIndex = getAdapterIndex(_gpu);
	if (arrayIndex == -1)
		return res;

	if (ADL_OK == ADL2_Overdrive_Caps(context, lpAdapterInfo[arrayIndex].iAdapterIndex, &iOverdriveSupported, &iOverdriveEnabled, &iOverdriveVersion))
	{
		if (iOverdriveSupported && iOverdriveVersion == 6)
			return getTemps_OD6(_gpu, arrayIndex);
		else if (iOverdriveSupported && iOverdriveVersion == 7)
			return getTemps_ODN(_gpu, arrayIndex);
		else
			LogF << "ADLUtils.getTemps: Unsupported OverDrive version [" << iOverdriveVersion << "]";
	}

	return res;
}

// overdrive 6 has been out since at least 2014, maybe earlier.
double ADLUtils::getTemps_OD6(int _gpu, int _arrayIndex)
{
	int temperature = 0;
	int adapterIndex = lpAdapterInfo[_arrayIndex].iAdapterIndex;
	ADLOD6ThermalControllerCaps thermalControllerCaps = {0};
	if (ADL_OK != ADL2_Overdrive6_ThermalController_Caps(context, adapterIndex, &thermalControllerCaps))
		LogF << "ADLUtils.getTemps_OD6: Failed to get thermal controller capabilities for GPU[" << _gpu << "]";
	else
		//Verifies that thermal controller exists on the GPU.
		if (ADL_OD6_TCCAPS_THERMAL_CONTROLLER != (thermalControllerCaps.iCapabilities & ADL_OD6_TCCAPS_THERMAL_CONTROLLER))
			LogF << "ADLUtils.getTemps_OD6: No temperature information for GPU[" << _gpu << "]";
		else
			if (ADL_OK != ADL2_Overdrive6_Temperature_Get(context, adapterIndex, &temperature))
				LogF << "ADLUtils.getTemps_OD6: Failed to get GPU temperature for GPU[" << _gpu << "]";

	return temperature / 1000.0;
}

// overdrive N is for newer GPUs, introduced sometime in 2016, I'm guessing.
double ADLUtils::getTemps_ODN(int _gpu, int _arrayIndex)
{
	int temperature = 0;
	int adapterIndex = lpAdapterInfo[_arrayIndex].iAdapterIndex;
	if (ADL_OK != ADL2_OverdriveN_Temperature_Get(context, adapterIndex, 1, &temperature))
		LogF << "ADLUtils.getTemps_ODN: Failed to get GPU temperature for GPU[" << _gpu << "]";
	else
	{
		LogF << "ADLUtils.getTemps_ODN: GPU[" << _gpu << "] temperature = " << temperature;
	}
	return temperature / 1000.0;
}

int ADLUtils::getFanSpeed(int _gpu)
{

	Guard l(x_temps);
	int res = 0;

	int arrayIndex = getAdapterIndex(_gpu);
	if (arrayIndex == -1)
		return res;

	if (ADL_OK == ADL2_Overdrive_Caps(context, lpAdapterInfo[arrayIndex].iAdapterIndex, &iOverdriveSupported, &iOverdriveEnabled, &iOverdriveVersion))
	{
		if (iOverdriveSupported && iOverdriveVersion == 6)
			return getFanSpeed_OD6(_gpu, arrayIndex);
		else if (iOverdriveSupported && iOverdriveVersion == 7)
			return getFanSpeed_ODN(_gpu, arrayIndex);
		else
			LogF << "ADLUtils.getFanSpeed: Unsupported OverDrive version [" << iOverdriveVersion << "]";
	}
	return res;
}


int ADLUtils::getFanSpeed_OD6(int _gpu, int _arrayIndex)
{
	int adapterIndex = lpAdapterInfo[_arrayIndex].iAdapterIndex;
	ADLOD6ThermalControllerCaps thermalControllerCaps = {0};
	if (ADL_OK != ADL2_Overdrive6_ThermalController_Caps(context, adapterIndex, &thermalControllerCaps))
	{
		LogF << "ADLUtils.getFanSpeed_OD6: Failed to get thermal controller capabilities for GPU[" << _gpu << "]";
		return 0;
	}

	//Verifies that fan speed controller exists on the GPU.
	if (ADL_OD6_TCCAPS_FANSPEED_CONTROL != (thermalControllerCaps.iCapabilities & ADL_OD6_TCCAPS_FANSPEED_CONTROL) ||
		ADL_OD6_TCCAPS_FANSPEED_RPM_READ != (thermalControllerCaps.iCapabilities & ADL_OD6_TCCAPS_FANSPEED_RPM_READ))
	{
		LogF << "ADLUtils.getFanSpeed_OD6: Error reading fan speed for GPU[" << _gpu << "]";
		return 0;
	}

	// read the fan speed
	ADLOD6FanSpeedInfo fanSpeedInfo = {0};
	ADL2_Overdrive6_FanSpeed_Get(context, adapterIndex, &fanSpeedInfo);
	return fanSpeedInfo.iFanSpeedRPM;
}


int ADLUtils::getFanSpeed_ODN(int _gpu, int _arrayIndex)
{
	ADLODNFanControl odNFanControl;
	memset(&odNFanControl, 0, sizeof(ADLODNFanControl));
	int adapterIndex = lpAdapterInfo[_arrayIndex].iAdapterIndex;

	if (ADL_OK != ADL2_OverdriveN_FanControl_Get(context, adapterIndex, &odNFanControl))
	{
		LogF << "ADLUtils.getFanSpeed_ODN: ADL2_OverdriveN_FanControl_Get failed for GPU[" << _gpu << "]";
		return 0;
	}
	else
		return odNFanControl.iCurrentFanSpeed;
}




