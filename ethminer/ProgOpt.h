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

#include <boost/property_tree/ptree.hpp>
#include <unordered_map>

class ProgOpt
{

public:

	static bool Load(std::string _config);
	static void SaveToDisk();
	static std::string Get(std::string _section, std::string _key, std::string _default);
	static std::string Get(std::string _section, std::string _key);
	static void Put(std::string _section, std::string _key, std::string _value);
	static void Put(std::string _section, std::string _key, uint64_t _value);
	static void beginUpdating();
	static void endUpdating();

	typedef std::unordered_map<std::string, std::string> defaults_t;

private:

	static boost::property_tree::iptree *m_tree;
	static defaults_t *m_defaults;
	static bool m_updating;

};
