
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

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/thread.hpp>
#include <json/json.h>

class UDPSocket {
public:

	using CommandRecvFn = boost::function<void(Json::Value const&)>;

	UDPSocket(unsigned _udpListen);
	~UDPSocket();
	void onCommandRecv(CommandRecvFn _handler);
	bool connected();
	void send_packet(Json::Value _v);
	void send_packet(Json::Value _v, int _returnPort);
	void send_packet(Json::Value _v, int _returnPort, boost::asio::ip::udp::endpoint _endpoint);
	void setCallerAsClient(int _returnPort, int _minerId);
	bool isCallerClient(int _returnPort);
	void disconnect(std::string _returnType);

private:

	void launchIOS();
	void start_receive();
	void handle_receive(const boost::system::error_code& error, std::size_t bytes_transferred);
	void handle_send(boost::shared_ptr<std::string> message, const boost::system::error_code& error, std::size_t bytes_transferred);

private:

	// event handler function
	CommandRecvFn m_onCommandRecv;

	static const int RECV_BUFF_SIZE = 512;

	boost::asio::io_service* m_ios;
	boost::asio::ip::udp::socket* m_socket;
	boost::asio::ip::udp::endpoint m_recv_endpoint;
	boost::array<char, RECV_BUFF_SIZE> m_recv_buffer;

	int m_listenport;
	int m_connection_returnport = 5226;
	boost::asio::ip::udp::endpoint m_connection_endpoint;
	int m_minerId;
	// this does not refer to a socket connection, but rather to a miner connection.
	bool m_connected;

};


