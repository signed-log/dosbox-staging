// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#if C_MODEM

#define ENET_IMPLEMENTATION

#include "misc_util.h"

#include <cassert>

#include "timer.h"

// Constants
constexpr int connection_timeout_ms = 5000;

const char* to_string(const SocketType socket_type)
{
	switch (socket_type) {
	case SocketType::Tcp: return "TCP";
	case SocketType::Enet: return "ENet";
	default: assert(false); return "Invalid SocketType value";
	}
}

// --- GENERIC NET INTERFACE -------------------------------------------------

NETClientSocket::NETClientSocket()
{
	// nothing
}

NETClientSocket::~NETClientSocket()
{
	// nothing
}

NETClientSocket* NETClientSocket::NETClientFactory(const SocketType socketType,
                                                   const char* destination,
                                                   const uint16_t port)
{
	switch (socketType) {
	case SocketType::Tcp: return new TCPClientSocket(destination, port);
	case SocketType::Enet: return new ENETClientSocket(destination, port);
	default: return nullptr;
	}
	return nullptr;
}

void NETClientSocket::FlushBuffer()
{
	if (sendbufferindex) {
		if (!SendArray(sendbuffer.data(), sendbufferindex))
			return;
		sendbufferindex = 0;
	}
}

void NETClientSocket::SetSendBufferSize(size_t n)
{
	sendbuffer.resize(n);
	assert(sendbuffer.size() == n);
	sendbufferindex = 0;
}

bool NETClientSocket::SendByteBuffered(uint8_t val)
{
	if (sendbuffer.empty())
		return false;

	// sanity check to prevent empty unsigned wrap-around
	assert(sendbuffer.size());
	if (sendbufferindex < (sendbuffer.size() - 1)) {
		sendbuffer[sendbufferindex] = val;
		sendbufferindex++;
		return true;
	}

	// sanity check to prevent index underflow
	assert(sendbufferindex < sendbuffer.size());

	// buffer is full, get rid of it
	sendbuffer[sendbufferindex] = val;
	sendbufferindex = 0;
	return SendArray(sendbuffer.data(), sendbuffer.size());
}

NETServerSocket::NETServerSocket() {}

NETServerSocket::~NETServerSocket() {}

void NETServerSocket::Close()
{
	// Discard any queued incoming connections
	while (auto accepted = Accept()) {
		delete accepted;
	}
}

NETServerSocket* NETServerSocket::NETServerFactory(const SocketType socketType,
                                                   const uint16_t port)
{
	switch (socketType) {
	case SocketType::Tcp: return new TCPServerSocket(port);
	case SocketType::Enet: return new ENETServerSocket(port);
	default: return nullptr;
	}
	return nullptr;
}

// --- ENet UDP NET INTERFACE ------------------------------------------------

class enet_manager_t {
public:
	enet_manager_t()
	{
		if (already_tried_once)
			return;
		already_tried_once = true;
		LOG_INFO("ENET: The reliable UDP networking subsystem version: %d.%d.%d",
		         ENET_VERSION_MAJOR, ENET_VERSION_MINOR, ENET_VERSION_PATCH);
		is_initialized = enet_initialize() == 0;
		if (is_initialized)
			LOG_INFO("ENET: Initialised successfully");
		else
			LOG_WARNING("ENET: failed to initialize ENet\n");
	}

	~enet_manager_t()
	{
		if (!is_initialized)
			return;

		assert(already_tried_once);
		enet_deinitialize();
		is_initialized = false;
		LOG_INFO("ENET: Shutting down the ENet subsystem");
	}

	bool IsInitialized() const { return is_initialized; }

private:
	bool already_tried_once = false;
	bool is_initialized = false;
};

bool NetWrapper_InitializeENET()
{
	static enet_manager_t enet_manager;
	return enet_manager.IsInitialized();
}

ENETServerSocket::ENETServerSocket(uint16_t port)
{
	if (!NetWrapper_InitializeENET())
		return;

	address.host = ENET_HOST_ANY;
	address.port = port;

	if (host) {
		LOG_MSG("ENET: Resetting server socket");
		enet_host_destroy(host);
		host = nullptr;
	}

	assert(!host);
	host = enet_host_create(&address, // create a host
	                        1, // only allow 1 client to connect
	                        1, // allow 1 channel to be used, 0
	                        0, // assume any amount of incoming bandwidth
	                        0  // assume any amount of outgoing bandwidth
	);
	if (host) {
		LOG_INFO("ENET: Server listening on port %d", port);
	} else {
		LOG_WARNING("ENET: Failed to create server on port %d", port);
		assert(!isopen);
		return;
	}
	isopen = true;
}

ENETServerSocket::~ENETServerSocket()
{
	// We don't destroy 'host' after passing it to a client, it needs to live.
	if (host && !nowClient) {
		assert(isopen);
		enet_host_destroy(host);
		host = nullptr;
		LOG_INFO("ENET: Stopping the server on port %u", address.port);
	}
	isopen = false;
}

// covert an ENet address to a string
static char *enet_address_to_string(const ENetAddress &address)
{
	static char ip_buf[INET_ADDRSTRLEN];
	enet_address_get_host_ip_new(&address, ip_buf, sizeof(ip_buf));
	return ip_buf;
}

NETClientSocket *ENETServerSocket::Accept()
{
	ENetEvent event;
	while (enet_host_service(host, &event, 0) > 0) {
		switch (event.type) {
		case ENET_EVENT_TYPE_CONNECT:
			// Log the connection's IP address and port
			LOG_INFO("ENET: Incoming connection from client %s:%u",
			         enet_address_to_string(event.peer->address),
			         event.peer->address.port);
			nowClient = true;
			return new ENETClientSocket(host);
			break;

		case ENET_EVENT_TYPE_RECEIVE:
			enet_packet_destroy(event.packet);
			break;

		case ENET_EVENT_TYPE_DISCONNECT:
		case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: isopen = false; break;

		default: break;
		}
	}

	return nullptr;
}

ENETClientSocket::ENETClientSocket(const char *destination, uint16_t port)
{
	if (!NetWrapper_InitializeENET())
		return;

	if (client) {
		LOG_MSG("ENET: Resetting connection");
		enet_host_destroy(client);
		client = nullptr;
	}
	assert(!client);
	client = enet_host_create(nullptr, // create a client host
	                          1,       // only allow 1 outgoing connection
	                          1,       // allow 1 channel to be used, 0
	                          0,       // assume any amount of incoming bandwidth
	                          0        // assume any amount of outgoing bandwidth
	);
	if (client == nullptr) {
		LOG_WARNING("ENET: Unable to create socket to %s:%u",
		            destination, port);
		return;
	}

	enet_address_set_host(&address, destination);
	address.port = port;
	peer = enet_host_connect(client, &address, 1, 0);
	if (peer) {
		LOG_INFO("ENET: Initiating connection to server %s:%u",
		         destination, port);
	} else {
		enet_host_destroy(client);
		client = nullptr;
		LOG_WARNING("ENET: Unable to connect to server %s:%u",
		            destination, port);
		return;
	}

#ifndef ENET_BLOCKING_CONNECT
	// Start connection timeout clock.
	connectStart = GetTicks();
	connecting   = true;
#else
	ENetEvent event;
	// Wait up to 5 seconds for the connection attempt to succeed.
	if (enet_host_service(client, &event, connection_timeout_ms) > 0 &&
	    event.type == ENET_EVENT_TYPE_CONNECT) {
		LOG_INFO("ENET: Established connection to server %s:%u",
		         destination, port);
	} else {
		LOG_WARNING("ENET: Failed connecting to server %s:%u",
		            destination, port);
		enet_peer_reset(peer);
		enet_host_destroy(client);
		client = nullptr;
		return;
	}
#endif

	isopen = true;
}

ENETClientSocket::ENETClientSocket(ENetHost *host)
{
	assert(host);
	client  = host;
	address = client->address;
	peer    = &client->peers[0];
	isopen  = true;
	LOG_INFO("ENET: Established connection to client %s:%u",
	         enet_address_to_string(peer->address), peer->address.port);
}

ENETClientSocket::~ENETClientSocket()
{
	if (isopen) {
		assert(peer);
		enet_peer_reset(peer);
		enet_host_destroy(client);
		LOG_INFO("ENET: Closed connection to client %s:%u",
		         enet_address_to_string(peer->address), peer->address.port);
		client = nullptr;
		isopen = false;
	}
}

SocketState ENETClientSocket::GetcharNonBlock(uint8_t &val)
{
	updateState();

	if (receiveBuffer.size()) {
		val = receiveBuffer.front();
		receiveBuffer.pop();
		return SocketState::Good;
	}

	return SocketState::Empty;
}

bool ENETClientSocket::Putchar(uint8_t val)
{
	updateState();

	const auto packet = enet_packet_create(&val, 1, ENET_PACKET_FLAG_RELIABLE);

	// Is the packet OK?
	if (packet == nullptr) {
		LOG_WARNING("ENET: Failed creating packet");
		return false;
	}

	// Did the packet send successfully?
	assert(peer);
	if (enet_peer_send(peer, 0, packet) < 0) {
		LOG_WARNING("ENET: Failed sending packet to peer %s:%u",
		            enet_address_to_string(peer->address),
		            peer->address.port);
		enet_packet_destroy(packet);
		return false;
	}

	updateState();
	return isopen;
}

bool ENETClientSocket::SendArray(const uint8_t *data, size_t n)
{
	updateState();

	// The UDP protocol sets a maximum packt size of 65535 bytes:
	// an 8-byte header and 65,527-byte payload.
	const auto packet_bytes = check_cast<uint16_t>(n);

	const auto packet = enet_packet_create(data, packet_bytes,
	                                       ENET_PACKET_FLAG_RELIABLE);

	// Is the packet OK?
	if (packet == nullptr) {
		LOG_WARNING("ENET: Failed creating %u-byte packet", packet_bytes);
		return false;
	}

	// Did the packet send successfully?
	assert(peer);
	if (enet_peer_send(peer, 0, packet) < 0) {
		LOG_WARNING("ENET: Failed sending %u-byte packet to peer %s:%u",
		            packet_bytes, enet_address_to_string(peer->address),
		            peer->address.port);
		enet_packet_destroy(packet);
		return false;
	}

	updateState();
	return isopen;
}

bool ENETClientSocket::ReceiveArray(uint8_t *data, size_t &n)
{
	size_t x = 0;

	// Prime the pump.
	updateState();

#if 0
	// This block is how the SDLNet documentation says the TCP code should behave.

	// The SDL TCP code doesn't wait if there is no pending data.
	if (receiveBuffer.empty()) {
	        n = 0;
	        return isopen;
	}

	// SDLNet_TCP_Recv says it blocks until it receives "n" bytes.
	// Based on the softmodem code, I'm not sure this is true.
	while (isopen && x < n) {
	        if (!receiveBuffer.empty()) {
	                data[x++] = receiveBuffer.front();
	                receiveBuffer.pop();
	        }
	        updateState();
	}
#else
	// This block is how softmodem.cpp seems to expect the code to behave.
	// Needless to say, not following the docs seems to work better.  :-)

	while (isopen && x < n && !receiveBuffer.empty()) {
		data[x++] = receiveBuffer.front();
		receiveBuffer.pop();
		updateState();
	}

	n = x;
#endif

	return isopen;
}

bool ENETClientSocket::GetRemoteAddressString(char *buffer)
{
	updateState();
	assert(buffer);
	enet_address_get_host_ip(&address, buffer, 16);
	return true;
}

void ENETClientSocket::updateState()
{
	if (!isopen || !client)
		return;

	ENetEvent event;
	while (enet_host_service(client, &event, 0) > 0) {
		switch (event.type) {
#ifndef ENET_BLOCKING_CONNECT
		case ENET_EVENT_TYPE_CONNECT:
			connecting = false;
			assert(event.peer);
			LOG_INFO("ENET: Established connection to server %s:%u",
			         enet_address_to_string(event.peer->address),
			         event.peer->address.port);
			break;
#endif
		case ENET_EVENT_TYPE_RECEIVE:
			assert(event.packet);
			for (size_t x = 0; x < event.packet->dataLength; x++) {
				receiveBuffer.push(event.packet->data[x]);
			}
			enet_packet_destroy(event.packet);
			break;

		case ENET_EVENT_TYPE_DISCONNECT:
		case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: isopen = false; break;

		default: break;
		}
	}

#ifndef ENET_BLOCKING_CONNECT
	if (connecting) {
		// Check for timeout.
		if (GetTicksSince(connectStart) > connection_timeout_ms) {
			assert(peer);
			LOG_WARNING("ENET: Timed out after %.1f seconds waiting for server %s:%u",
			            connection_timeout_ms / 1000.0,
			            enet_address_to_string(peer->address),
			            peer->address.port);
			enet_peer_reset(peer);
			enet_host_destroy(client);
			client = nullptr;
			connecting = false;
			isopen     = false;
		}
	}
#endif
}

// --- TCP NET INTERFACE -----------------------------------------------------

class sdl_net_manager_t {
public:
	sdl_net_manager_t()
	{
		if (already_tried_once)
			return;
		already_tried_once = true;

		is_initialized = SDLNet_Init() != -1;
		if (is_initialized)
			LOG_INFO("SDLNET: Initialised SDL network subsystem");
		else
			LOG_WARNING("SDLNET: failed to initialize SDL network subsystem: %s\n",
			            SDLNet_GetError());
	}

	~sdl_net_manager_t()
	{
		if (!is_initialized)
			return;

		assert(already_tried_once);
		SDLNet_Quit();
		LOG_INFO("SDLNET: Shutdown SDL network subsystem");
	}

	bool IsInitialized() const { return is_initialized; }

private:
	bool already_tried_once = false;
	bool is_initialized = false;
};

bool NetWrapper_InitializeSDLNet()
{
	static sdl_net_manager_t sdl_net_manager;
	return sdl_net_manager.IsInitialized();
}

#ifdef NATIVESOCKETS
TCPClientSocket::TCPClientSocket(int platformsocket)
{
	if (!NetWrapper_InitializeSDLNet())
		return;

	nativetcpstruct = new _TCPsocketX;
	mysock = (TCPsocket)nativetcpstruct;

	// fill the SDL socket manually
	nativetcpstruct->ready = 0;
	nativetcpstruct->sflag = 0;
	nativetcpstruct->channel = platformsocket;
	sockaddr_in		sa;
	socklen_t		sz;
	sz=sizeof(sa);
	if(getpeername(platformsocket, (sockaddr *)(&sa), &sz)==0) {
		nativetcpstruct->remoteAddress.host = /*ntohl(*/sa.sin_addr.s_addr;//);
		nativetcpstruct->remoteAddress.port = /*ntohs(*/sa.sin_port;//);
	}
	else {
		mysock = nullptr;
		return;
	}
	sz=sizeof(sa);
	if(getsockname(platformsocket, (sockaddr *)(&sa), &sz)==0) {
		(nativetcpstruct)->localAddress.host = /*ntohl(*/ sa.sin_addr.s_addr; //);
		(nativetcpstruct)->localAddress.port = /*ntohs(*/ sa.sin_port; //);
	}
	else {
		mysock = nullptr;
		return;
	}
	if(mysock) {
		listensocketset = SDLNet_AllocSocketSet(1);
		if(!listensocketset) return;
		SDLNet_TCP_AddSocket(listensocketset, mysock);
		isopen=true;
		return;
	}
	return;
}
#endif // NATIVESOCKETS

TCPClientSocket::TCPClientSocket(TCPsocket source)
{
	if (!NetWrapper_InitializeSDLNet())
		return;

	if(source!=nullptr) {
		mysock = source;
		listensocketset = SDLNet_AllocSocketSet(1);
		if(!listensocketset) return;
		SDLNet_TCP_AddSocket(listensocketset, source);

		isopen=true;
	}
}

TCPClientSocket::TCPClientSocket(const char *destination, uint16_t port)
{
	if (!NetWrapper_InitializeSDLNet())
		return;

	IPaddress openip;
	//Ancient versions of SDL_net had this as char*. People still appear to be using this one.
	if (!SDLNet_ResolveHost(&openip, destination, port)) {
		listensocketset = SDLNet_AllocSocketSet(1);
		if(!listensocketset) return;
		mysock = SDLNet_TCP_Open(&openip);
		if (!mysock)
			return;
		SDLNet_TCP_AddSocket(listensocketset, mysock);
		isopen=true;
	}
}

TCPClientSocket::~TCPClientSocket()
{
#ifdef NATIVESOCKETS
	if (nativetcpstruct) { //-V809
		delete nativetcpstruct;
	}
	// Very important else. If we're using a native TCP socket, we can't call SDL's close.
	// nativetcpstruct == mysock so it's a double free and it wasn't created by SDL to begin with
	else
#endif
	if(mysock) {
		SDLNet_TCP_Close(mysock);
		LOG_INFO("SDLNET: Closed client TCP listening socket");
	}

	if (listensocketset) {
		SDLNet_FreeSocketSet(listensocketset);
	}
}

bool TCPClientSocket::GetRemoteAddressString(char *buffer)
{
	IPaddress *remote_ip;
	uint8_t b1, b2, b3, b4;
	remote_ip=SDLNet_TCP_GetPeerAddress(mysock);
	if(!remote_ip) return false;
	b4=remote_ip->host>>24;
	b3=(remote_ip->host>>16)&0xff;
	b2=(remote_ip->host>>8)&0xff;
	b1=remote_ip->host&0xff;
	sprintf(buffer, "%" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8, b1, b2, b3, b4);
	return true;
}

bool TCPClientSocket::ReceiveArray(uint8_t *data, size_t &n)
{
	assertm(n <= static_cast<size_t>(std::numeric_limits<int>::max()),
	        "SDL_net can't handle more bytes at a time.");
	assert(data);
	if (SDLNet_CheckSockets(listensocketset, 0)) {
		const int result = SDLNet_TCP_Recv(mysock, data, static_cast<int>(n));
		if(result < 1) {
			isopen = false;
			n = 0;
			return false;
		} else {
			n = result;
			return true;
		}
	} else {
		n = 0;
		return true;
	}
}

SocketState TCPClientSocket::GetcharNonBlock(uint8_t &val)
{
	SocketState state = SocketState::Empty;
	if(SDLNet_CheckSockets(listensocketset,0))
	{
		if (SDLNet_TCP_Recv(mysock, &val, 1) == 1)
			state = SocketState::Good;
		else {
			isopen = false;
			state = SocketState::Closed;
		}
	}
	return state;
}

bool TCPClientSocket::Putchar(uint8_t val)
{
	return SendArray(&val, 1);
}

bool TCPClientSocket::SendArray(const uint8_t *data, const size_t n)
{
	assertm(n <= static_cast<size_t>(std::numeric_limits<int>::max()),
	        "SDL_net can't handle more bytes at a time.");
	assert(data);
	if (SDLNet_TCP_Send(mysock, data, static_cast<int>(n))
	    != static_cast<int>(n)) {
		isopen = false;
		return false;
	}
	return true;
}

TCPServerSocket::TCPServerSocket(const uint16_t port)
{
	isopen = false;
	mysock = nullptr;

	if (!NetWrapper_InitializeSDLNet())
		return;

	if (port) {
		IPaddress listen_ip;
		SDLNet_ResolveHost(&listen_ip, nullptr, port);
		mysock = SDLNet_TCP_Open(&listen_ip);
		if (!mysock)
			return;
	} else {
		return;
	}
	isopen = true;
}

TCPServerSocket::~TCPServerSocket()
{
	if (mysock) {
		SDLNet_TCP_Close(mysock);
		LOG_INFO("SDLNET: closed server TCP listening socket");
	}
}

NETClientSocket *TCPServerSocket::Accept()
{
	TCPsocket new_tcpsock;

	new_tcpsock=SDLNet_TCP_Accept(mysock);
	if(!new_tcpsock) {
		//printf("SDLNet_TCP_Accept: %s\n", SDLNet_GetError());
		return nullptr;
	}
	
	return new TCPClientSocket(new_tcpsock);
}

#endif // C_MODEM
