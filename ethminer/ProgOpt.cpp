
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

#include <ethminer/ProgOpt.h>
#include <ethminer/ini_parser_ex.hpp>
#include <string>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <ethminer/Misc.h>
#include <ethminer/MultiLog.h>

namespace pt = boost::property_tree;
namespace fs = boost::filesystem;

// static intializers
pt::iptree *ProgOpt::m_tree;
bool ProgOpt::m_updating = false;
ProgOpt::defaults_t *ProgOpt::m_defaults;


bool ProgOpt::Load(std::string _config)
{
	m_tree = new pt::iptree;
	m_tree->clear();

	m_defaults = new ProgOpt::defaults_t;
	m_defaults->clear();

	if (_config != "" && !fileExists(_config))
	{
		LogB << "ERROR! Invalid --config parameter : '" << _config << "'.  File does not exist!";
		return false;
	}

	fs::path path;
	if (_config != "")
		path = _config;
	else
	{
		path = getAppDataFolder();
		path = path / "tokenminer.ini";
		if (!fs::exists(path))
		{
			LogB << "Error! Unable to read program settings from " << path.generic_string();
			return false;
		}
	}
	try
	{
		pt::read_ini_ex(path.generic_string(), *m_tree);
		
		// set up sensible defaults for various settings. note that emplace does
		// not overwrite existing values.
		m_defaults->emplace("ThermalProtection.TempProvider", "amd_adl");
		m_defaults->emplace("ThermalProtection.ThrottleTemp", "80");
		m_defaults->emplace("ThermalProtection.ShutDown", "20");

		m_defaults->emplace("Node.Host", "127.0.0.1");
		m_defaults->emplace("Node.RPCPort", "8545");

	}
	catch (std::exception const& _e)
	{
		LogB << "Exception: ProgOpt::Load - " << _e.what();
		return false;
	}

	return true;
}

void ProgOpt::SaveToDisk()
{
	fs::path path = getAppDataFolder();
	path = path / "tokenminer.ini";
	pt::write_ini(path.string(), *m_tree);
}

std::string ProgOpt::Get(std::string _section, std::string _key, std::string _default)
{
	return m_tree->get(_section + "." + _key, _default);
}

std::string ProgOpt::Get(std::string _section, std::string _key)
{
	std::string defKey = _section + "." + _key;
	defaults_t::const_iterator def = m_defaults->find(defKey);
	if (def == m_defaults->end())
		return m_tree->get(defKey, "");
	else
		return m_tree->get(defKey, def->second);
}

void ProgOpt::Put(std::string _section, std::string _key, std::string _value)
{
	m_tree->put(_section + "." + _key, _value);
	if (!m_updating)
		SaveToDisk();
}

void ProgOpt::Put(std::string _section, std::string _key, uint64_t _value)
{
	Put(_section, _key, boost::lexical_cast<std::string>(_value));
}

void ProgOpt::beginUpdating()
{
	m_updating = true;
}

void ProgOpt::endUpdating()
{
	m_updating = false;
	SaveToDisk();
}
