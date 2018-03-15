
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

#include "UDPSocket.h"

#include <iostream>
#include <string>
#include <thread>
#include <libdevcore/Guards.h>
#include <ethminer/MultiLog.h>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::udp;

/*-----------------------------------------------------------------------------------
* constructor
*----------------------------------------------------------------------------------*/
UDPSocket::UDPSocket(unsigned _udpListen) :
	m_onCommandRecv(NULL), m_connected(false), m_listenport(_udpListen)
{
	launchIOS();
}

/*-----------------------------------------------------------------------------------
* destructor
*----------------------------------------------------------------------------------*/
UDPSocket::~UDPSocket()
{
	m_ios->stop();
}

/*-----------------------------------------------------------------------------------
* launchIOS
*----------------------------------------------------------------------------------*/
void UDPSocket::launchIOS()
{

	boost::thread bt([&] () {
		try
		{
			m_ios = new io_service();
			m_socket = new udp::socket(*m_ios, udp::endpoint(udp::v4(), m_listenport));
			while (true) {
				try {
					start_receive();
					m_ios->run();
					break;
				} catch (std::exception& e) {
					LogB << "UDPSocket error : " << e.what();
					this_thread::sleep_for(chrono::milliseconds(3000));
					m_ios->reset();
				}
			}
		}
		catch (std::exception& e)
		{
			LogB << "UDPSocket::launchIOS : Error opening UDP port " << m_listenport << ". " << e.what();
		}
	});
}

/*-----------------------------------------------------------------------------------
* start_receive
*----------------------------------------------------------------------------------*/
void UDPSocket::start_receive()
{
	m_socket->async_receive_from(
		buffer(m_recv_buffer), m_recv_endpoint,
		boost::bind(&UDPSocket::handle_receive, this,
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred));
}

/*-----------------------------------------------------------------------------------
* handle_receive
*----------------------------------------------------------------------------------*/
void UDPSocket::handle_receive(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	(void) bytes_transferred;
	if (!error || error == error::message_size)
	{
		// 'message_size' means our input buffer wasn't big enough ... extra data was discarded.

		if (m_onCommandRecv)
		{
			Json::Reader reader;
			Json::Value value;
			if (reader.parse(m_recv_buffer.begin(), m_recv_buffer.end(), value))
				m_onCommandRecv(value);
			else
				LogB << "UDPSocket::handle_receive - JSON parsing error";
		}

	}
	else if (error == boost::system::errc::connection_refused || boost::system::errc::connection_reset)
	{
		// this can happen on Windows, when our peer is on the same machine, but stops listening.  don't know why it comes 
		// to the handle_receive and not the handle_send handler.  just ignore it since our keep-alive timer will eventually
		// cancel all reporting.
	}
	else
	{
		LogB << "UDPSocket::handle_receive error : " << error.category().name() << " " << error.message() << ":" << error.value();
	}

	start_receive();

}


/*-----------------------------------------------------------------------------------
* onCommandRecv
*----------------------------------------------------------------------------------*/
void UDPSocket::onCommandRecv(CommandRecvFn _handler) 
{ 
	m_onCommandRecv = _handler; 
}

/*-----------------------------------------------------------------------------------
* connected
*----------------------------------------------------------------------------------*/
bool UDPSocket::connected() 
{ 
	return m_connected; 
}

/*-----------------------------------------------------------------------------------
* send_packet
*----------------------------------------------------------------------------------*/
void UDPSocket::send_packet(Json::Value _v)
{
	if (!m_connected)
	{
		LogS << "UDPSocket::send_packet called but no miner connection.";
		return;
	}
	send_packet(_v, m_connection_returnport, m_connection_endpoint);
}

/*-----------------------------------------------------------------------------------
* send_packet
*----------------------------------------------------------------------------------*/
void UDPSocket::send_packet(Json::Value _v, int _returnPort)
{
	send_packet(_v, _returnPort, m_recv_endpoint);
}

/*-----------------------------------------------------------------------------------
* send_packet
*----------------------------------------------------------------------------------*/
void UDPSocket::send_packet(Json::Value _v, int _returnPort, boost::asio::ip::udp::endpoint _endpoint)
{
	udp::endpoint _send_endpoint = _endpoint;
	_send_endpoint.port(_returnPort);
	Json::FastWriter fw;
	fw.omitEndingLineFeed();
	_v["miner_id"] = m_minerId;
	boost::shared_ptr<std::string> message(new std::string(fw.write(_v)));
	LogF << "UDPSocket::send_packet : " << *message;
	// we're passing message to handle_send just to keep the memory valid long enough.
	m_socket->async_send_to(buffer(*message),
							_send_endpoint,
							boost::bind(&UDPSocket::handle_send, this,
										message,
										boost::asio::placeholders::error,
										boost::asio::placeholders::bytes_transferred));
}

/*-----------------------------------------------------------------------------------
* handle_send
*----------------------------------------------------------------------------------*/
void UDPSocket::handle_send(boost::shared_ptr<std::string> message, const boost::system::error_code& error, std::size_t bytes_transferred)
{
	(void) bytes_transferred;
	if (error)
		LogB << "UDPSocket::handle_send error : " << error.category().name() << " " << error.message() << ":" << error.value();
}

/*-----------------------------------------------------------------------------------
* setCallerAsClient
*----------------------------------------------------------------------------------*/
void UDPSocket::setCallerAsClient(int _returnPort, int _minerId)
{
	m_connection_returnport = _returnPort;
	m_connection_endpoint = m_recv_endpoint;
	m_minerId = _minerId;
	m_connected = true;
}

/*-----------------------------------------------------------------------------------
* isCallerClient
*----------------------------------------------------------------------------------*/
bool UDPSocket::isCallerClient(int _returnPort)
{
	// is the current caller our connected client
	return m_connected && (m_recv_endpoint.address() == m_connection_endpoint.address()) && (_returnPort == m_connection_returnport);
}

/*-----------------------------------------------------------------------------------
* disconnect
*----------------------------------------------------------------------------------*/
void UDPSocket::disconnect(std::string _returnType)
{
	if (_returnType != "")
	{
		Json::Value v(Json::objectValue);
		v["data_id"] = "disconnect";
		v["type"] = _returnType;
		send_packet(v);
	}
	m_connected = false;
}



