/*
 * Copyright (c) 2016-2018  Moddable Tech, Inc.
 *
 *   This file is part of the Moddable SDK Runtime.
 * 
 *   The Moddable SDK Runtime is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 * 
 *   The Moddable SDK Runtime is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 * 
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with the Moddable SDK Runtime.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "xsmc.h"
#include "xsHost.h"
#include "modInstrumentation.h"
#include "mc.xs.h"			// for xsID_ values
#include "mc.defines.h"

#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/udp.h"
#include "lwip/raw.h"

#include "modSocket.h"
#include "modLwipSafe.h"

#ifndef MODDEF_SOCKET_READQUEUE
	#define MODDEF_SOCKET_READQUEUE (6)
#endif
#ifndef MODDEF_SOCKET_LISTENERQUEUE
	#define MODDEF_SOCKET_LISTENERQUEUE (4)
#endif

#ifdef mxDebug
	extern uint8_t fxInNetworkDebugLoop(xsMachine *the);
#endif

#if ESP32
	#include "esp_wifi.h"
	typedef int8_t int8;
	typedef uint8_t uint8;
	typedef int16_t int16;
	typedef uint16_t uint16;

	typedef ip_addr_t ip_addr;
#else
	#define IP_ADDR4 IP4_ADDR
#endif

typedef struct xsSocketRecord xsSocketRecord;
typedef xsSocketRecord *xsSocket;

#define kTCP (0)
#define kUDP (1)
#define kRAW (2)
#define kTCPListener (3)

#define kPendingConnect (1 << 0)
#define kPendingError (1 << 1)
#define kPendingDisconnect (1 << 2)
#define kPendingReceive (1 << 3)
#define kPendingSent (1 << 4)
#define kPendingAcceptListener (1 << 5)
#define kPendingClose (1 << 6)
#define kPendingOutput (1 << 7)

struct xsSocketUDPRemoteRecord {
	uint16_t			port;
	ip_addr_t			address;
};
typedef struct xsSocketUDPRemoteRecord xsSocketUDPRemoteRecord;
typedef xsSocketUDPRemoteRecord *xsSocketUDPRemote;

#define kReadQueueLength MODDEF_SOCKET_READQUEUE
struct xsSocketRecord {
	xsMachine			*the;

	xsSlot				obj;
	struct tcp_pcb		*skt;

	int8				useCount;
	uint8				kind;
	uint8				pending;
	uint8				writeDisabled;
	uint8				constructed;

	// above here same as xsListenerRecord

	struct udp_pcb		*udp;
	struct raw_pcb		*raw;

	struct pbuf			*reader[kReadQueueLength];

	uint32_t			outstandingSent;

	unsigned char		*buf;
	struct pbuf			*pb;
	struct pbuf			*pbWalker;
	uint16				bufpos;
	uint16				buflen;
	uint16				port;
	uint8				suspended;

	uint8				suspendedError;		// could overload suspended
	uint8				suspendedDisconnect;
	uint16				suspendedBufpos;
	struct pbuf			*suspendedBuf;
	struct pbuf			*suspendedFragment;

	xsSocketUDPRemoteRecord
						remote[1];
};

typedef struct xsListenerRecord xsListenerRecord;
typedef xsListenerRecord *xsListener;

#define kListenerPendingSockets MODDEF_SOCKET_LISTENERQUEUE
struct xsListenerRecord {
	xsMachine			*the;

	xsSlot				obj;
	struct tcp_pcb		*skt;

	int8				useCount;
	uint8				kind;
	uint8				pending;
	uint8				writeDisabled;
	uint8				constructed;

	// above here same as xsSocketRecord

	xsSocket			accept[kListenerPendingSockets];
};

static xsSocket gSockets;		// N.B. this list contains both sockets and listeners

void xs_socket_destructor(void *data);

static void socketSetPending(xsSocket xss, uint8_t pending);
static void socketClearPending(void *the, void *refcon, uint8_t *message, uint16_t messageLength);

static void configureSocketTCP(xsMachine *the, xsSocket xss);

static void socketMsgConnect(xsSocket xss);
static void socketMsgDisconnect(xsSocket xss);
static void socketMsgError(xsSocket xss);
static void socketMsgDataReceived(xsSocket xss);
static void socketMsgDataSent(xsSocket xss);

#if ESP32
	static void didFindDNS(const char *name, const ip_addr_t *ipaddr, void *arg);
#else
	static void didFindDNS(const char *name, ip_addr_t *ipaddr, void *arg);
#endif
static void didError(void *arg, err_t err);
static err_t didConnect(void * arg, struct tcp_pcb * tpcb, err_t err);
static err_t didReceive(void * arg, struct tcp_pcb * pcb, struct pbuf * p, err_t err);
static err_t didSend(void *arg, struct tcp_pcb *pcb, u16_t len);
static void didReceiveUDP(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
#if ESP32
	static u8_t didReceiveRAW(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr);
#else
	static u8_t didReceiveRAW(void *arg, struct raw_pcb *pcb, struct pbuf *p, ip_addr_t *addr);
#endif

static uint8 parseAddress(char *address, uint8 *ip);

static void socketUpUseCount(xsMachine *the, xsSocket xss)
{
	modCriticalSectionBegin();
	xss->useCount += 1;
	modCriticalSectionEnd();
}

static int socketDownUseCount(xsMachine *the, xsSocket xss)
{
	xsDestructor destructor;

	modCriticalSectionBegin();
	int8 useCount = --xss->useCount;
	if (useCount > 0) {
		modCriticalSectionEnd();
		return 0;
	}

	xss->pending |= kPendingClose;
	modCriticalSectionEnd();
	destructor = xsGetHostDestructor(xss->obj);
	xsmcSetHostData(xss->obj, NULL);
	xsForget(xss->obj);
	(*destructor)(xss);
	return 1;
}

void xs_socket(xsMachine *the)
{
	xsSocket xss;
	ip_addr_t ipaddr;
	int port = 0;
	err_t err;
	char temp[DNS_MAX_NAME_LENGTH];
	unsigned char ip[4];
	int len, i;
	unsigned char multicastIP[4];
	int ttl = 0;

	xsmcVars(2);
	if (xsmcHas(xsArg(0), xsID_listener)) {
		xsListener xsl;
		xsmcGet(xsVar(0), xsArg(0), xsID_listener);
		xsl = xsmcGetHostData(xsVar(0));

		modCriticalSectionBegin();

		for (i = 0; i < kListenerPendingSockets; i++) {
			if (xsl->accept[i]) {
				xss = xsl->accept[i];
				xsl->accept[i] = NULL;

				modCriticalSectionEnd();

				xss->obj = xsThis;
				xsmcSetHostData(xsThis, xss);
				xss->constructed = true;
				xsRemember(xss->obj);

				xss->skt->so_options |= SOF_REUSEADDR;
				configureSocketTCP(the, xss);

				tcp_recv(xss->skt, didReceive);

				socketUpUseCount(the, xss);

				if (xss->pending) {
					uint8_t pending;
					modCriticalSectionBegin();
						pending = xss->pending;
						xss->pending = 0;
					modCriticalSectionEnd();
					socketSetPending(xss, pending);
				}

				socketDownUseCount(xss->the, xss);
				return;
			}
		}

		modCriticalSectionEnd();
		xsUnknownError("no socket avaiable from listener");
	}

	// allocate socket
	xss = c_calloc(1, sizeof(xsSocketRecord) + (sizeof(xsSocketUDPRemoteRecord) * (kReadQueueLength - 1)));
	if (!xss)
		xsUnknownError("no memory for socket record");

	xss->the = the;
	xss->obj = xsThis;
	xss->constructed = true;
	xss->useCount = 1;
	xsmcSetHostData(xsThis, xss);
	xsRemember(xss->obj);

	modInstrumentationAdjust(NetworkSockets, 1);

	// determine socket kind
	xss->kind = kTCP;
	if (xsmcHas(xsArg(0), xsID_kind)) {
		char *kind;

		xsmcGet(xsVar(0), xsArg(0), xsID_kind);
		kind = xsmcToString(xsVar(0));
		if (0 == c_strcmp(kind, "TCP"))
			;
		else if (0 == c_strcmp(kind, "UDP")) {
			xss->kind = kUDP;
			if (xsmcHas(xsArg(0), xsID_multicast)) {
				xsmcGet(xsVar(0), xsArg(0), xsID_multicast);
				xsmcToStringBuffer(xsVar(0), temp, sizeof(temp));
				if (!parseAddress(temp, multicastIP))
					xsUnknownError("invalid multicast IP address");
				if ((255 != multicastIP[0]) || (255 != multicastIP[1]) || (255 != multicastIP[2]) || (255 != multicastIP[3])) {		// ignore broadcast address (255.255.255.255) as lwip fails when trying to use it for multicast
					ttl = 1;
					if (xsmcHas(xsArg(0), xsID_ttl)) {
						xsmcGet(xsVar(0), xsArg(0), xsID_ttl);
						ttl = xsmcToInteger(xsVar(0));
					}
				}
			}
		}
		else if (0 == c_strcmp(kind, "RAW"))
			xss->kind = kRAW;
		else
			xsUnknownError("invalid socket kind");
	}

	// prepare inputs
	if (xsmcHas(xsArg(0), xsID_port)) {
		xsmcGet(xsVar(0), xsArg(0), xsID_port);
		port = xsmcToInteger(xsVar(0));
	}

	xss->port = port;
	if (kTCP == xss->kind)
		xss->skt = tcp_new_safe();
	else if (kUDP == xss->kind)
		xss->udp = udp_new_safe();
	else if (kRAW == xss->kind) {
		xsmcGet(xsVar(0), xsArg(0), xsID_protocol);
		xss->raw = raw_new(xsmcToInteger(xsVar(0)));
	}

	if (!xss->skt && !xss->udp && !xss->raw)
		xsUnknownError("failed to allocate socket");

	if (kTCP == xss->kind) {
		xss->skt->so_options |= SOF_REUSEADDR;
		err = tcp_bind_safe(xss->skt, IP_ADDR_ANY, 0);
	}
	else if (kUDP == xss->kind)
		err = udp_bind_safe(xss->udp, IP_ADDR_ANY, xss->port);
	else if (kRAW == xss->kind)
		err = raw_bind(xss->raw, IP_ADDR_ANY);
	if (err)
		xsUnknownError("socket bind failed");

	if (kTCP == xss->kind) {
		tcp_arg(xss->skt, xss);
		tcp_recv(xss->skt, didReceive);
		tcp_sent(xss->skt, didSend);
		tcp_err(xss->skt, didError);
	}
	else if (kUDP == xss->kind) {
		udp_recv(xss->udp, (udp_recv_fn)didReceiveUDP, xss);

		if (ttl) {
			ip_addr_t ifaddr;
	#if ESP32
			tcpip_adapter_ip_info_t info;
			tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &info);
			ifaddr.u_addr.ip4 = info.ip;
	#else
			struct ip_info staIpInfo;
			wifi_get_ip_info(0, &staIpInfo);		// 0 == STATION_IF
			ifaddr.addr = staIpInfo.ip.addr;
	#endif
			ip_addr_t multicast_addr;
			IP_ADDR4(&multicast_addr, multicastIP[0], multicastIP[1], multicastIP[2], multicastIP[3]);
			igmp_joingroup(&ifaddr, &multicast_addr);

			IP_ADDR4(&(xss->udp)->multicast_ip, multicastIP[0], multicastIP[1], multicastIP[2], multicastIP[3]);
			xss->udp->ttl = 1;
		}
	}
	else if (kRAW == xss->kind)
		raw_recv(xss->raw, didReceiveRAW, xss);

	if ((kUDP == xss->kind) || (kRAW == xss->kind))
		return;

	configureSocketTCP(the, xss);

	if (xsmcHas(xsArg(0), xsID_host)) {
		xsmcGet(xsVar(0), xsArg(0), xsID_host);
		xsmcToStringBuffer(xsVar(0), temp, sizeof(temp));
		ip_addr_t resolved;
		if (ERR_OK == dns_gethostbyname_safe(temp, &resolved, didFindDNS, xss)) {
#if LWIP_IPV4 && LWIP_IPV6
			ip[0] = ip4_addr1(&resolved.u_addr.ip4);
			ip[1] = ip4_addr2(&resolved.u_addr.ip4);
			ip[2] = ip4_addr3(&resolved.u_addr.ip4);
			ip[3] = ip4_addr4(&resolved.u_addr.ip4);
#else
			ip[0] = ip4_addr1(&resolved);
			ip[1] = ip4_addr2(&resolved);
			ip[2] = ip4_addr3(&resolved);
			ip[3] = ip4_addr4(&resolved);
#endif
		}
		else
			return;
	}
	else
	if (xsmcHas(xsArg(0), xsID_address)) {
		xsmcGet(xsVar(0), xsArg(0), xsID_address);
		xsmcToStringBuffer(xsVar(0), temp, sizeof(temp));
		if (!parseAddress(temp, ip))
			xsUnknownError("invalid IP address");
	}
	else
		xsUnknownError("invalid dictionary");

	IP_ADDR4(&ipaddr, ip[0], ip[1], ip[2], ip[3]);
	err = tcp_connect_safe(xss->skt, &ipaddr, port, didConnect);
	if (err)
		xsUnknownError("socket connect failed");
}

void xs_socket_destructor(void *data)
{
	xsSocket xss = data;
	unsigned char i;

	if (!xss) return;

	if (xss->skt) {
		tcp_recv(xss->skt, NULL);
		tcp_sent(xss->skt, NULL);
		tcp_err(xss->skt, NULL);
		tcp_close_safe(xss->skt);
	}

	if (xss->udp) {
		udp_recv(xss->udp, NULL, NULL);
		udp_remove_safe(xss->udp);
	}

	if (xss->raw) {
		raw_recv(xss->raw, NULL, NULL);
		raw_remove(xss->raw);
	}

	for (i = 0; i < kReadQueueLength; i++) {
		if (xss->reader[i])
			pbuf_free_safe(xss->reader[i]);
	}

	c_free(xss);

	modInstrumentationAdjust(NetworkSockets, -1);
}

void xs_socket_close(xsMachine *the)
{
	xsSocket xss = xsmcGetHostData(xsThis);

	if (NULL == xss)
		xsUnknownError("close on closed socket");

	if (!(xss->pending & kPendingClose))
		socketSetPending(xss, kPendingClose);
}

void xs_socket_get(xsMachine *the)
{
	xsSocket xss = xsmcGetHostData(xsThis);
	const char *name = xsmcToString(xsArg(0));

	if (0 == c_strcmp(name, "REMOTE_IP")) {
		char *out;
		ip_addr_t remote_ip = xss->skt->remote_ip;

		xsResult = xsStringBuffer(NULL, 4 * 5);
		out = xsmcToString(xsResult);

	#if LWIP_IPV4 && LWIP_IPV6
		itoa(ip4_addr1(&remote_ip.u_addr.ip4), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr2(&remote_ip.u_addr.ip4), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr3(&remote_ip.u_addr.ip4), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr4(&remote_ip.u_addr.ip4), out, 10); out += strlen(out); *out = 0;
	#else
		itoa(ip4_addr1(&remote_ip), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr2(&remote_ip), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr3(&remote_ip), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr4(&remote_ip), out, 10); out += strlen(out); *out = 0;
	#endif
	}
}

void xs_socket_read(xsMachine *the)
{
	xsSocket xss = xsmcGetHostData(xsThis);
	xsType dstType;
	int argc = xsmcArgc;
	uint16 srcBytes;
	unsigned char *srcData;

	if (NULL == xss)
		xsUnknownError("read on closed socket");

	if (!xss->buf || (xss->bufpos >= xss->buflen) || xss->suspended) {
		if (0 == argc)
			xsResult = xsInteger(0);
		else
			xsUnknownError("nothing to read");
		return;
	}

	srcData = xss->bufpos + (unsigned char *)xss->buf;
	srcBytes = xss->buflen - xss->bufpos;

	if (0 == argc) {
		xsResult = xsInteger(srcBytes);
		return;
	}

	// address limiter argument (count or terminator)
	if (argc > 1) {
		xsType limiterType = xsmcTypeOf(xsArg(1));
		if ((xsNumberType == limiterType) || (xsIntegerType == limiterType)) {
			uint16 count = xsmcToInteger(xsArg(1));
			if (count < srcBytes)
				srcBytes = count;
		}
		else
		if (xsStringType == limiterType) {
			char *str = xsmcToString(xsArg(1));
			char terminator = c_read8(str);
			if (terminator) {
				unsigned char *t = c_strchr(srcData, terminator);
				if (t) {
					uint16 count = (t - srcData) + 1;		// terminator included in result
					if (count < srcBytes)
						srcBytes = count;
				}
			}
		}
		else if (xsUndefinedType == limiterType)
			;
	}

	// generate output
	dstType = xsmcTypeOf(xsArg(0));

	if (xsNullType == dstType)
		xsResult = xsInteger(srcBytes);
	else if (xsReferenceType == dstType) {
		xsSlot *s1, *s2;

		s1 = &xsArg(0);

		xsmcVars(1);
		xsmcGet(xsVar(0), xsGlobal, xsID_String);
		s2 = &xsVar(0);
		if (s1->data[2] == s2->data[2])		//@@
			xsResult = xsStringBuffer(srcData, srcBytes);
		else {
			xsmcGet(xsVar(0), xsGlobal, xsID_Number);
			s2 = &xsVar(0);
			if (s1->data[2] == s2->data[2]) {		//@@
				xsResult = xsInteger(*srcData);
				srcBytes = 1;
			}
			else {
				xsmcGet(xsVar(0), xsGlobal, xsID_ArrayBuffer);
				s2 = &xsVar(0);
				if (s1->data[2] == s2->data[2])		//@@
					xsResult = xsArrayBuffer(srcData, srcBytes);
				else
					xsUnknownError("unsupported output type");
			}
		}
	}

	xss->bufpos += srcBytes;

	if (xss->bufpos == xss->buflen) {
		if (xss->pbWalker->next)
			socketSetPending(xss, kPendingReceive);
		else {
			pbuf_free_safe(xss->pb);
			xss->pb = NULL;
			xss->pbWalker = NULL;

			xss->bufpos = xss->buflen = 0;
			xss->buf = NULL;

			if (xss->reader[0])
				socketSetPending(xss, kPendingReceive);
			else if (xss->suspendedDisconnect)
				socketSetPending(xss, kPendingDisconnect);
		}
	}
}

void xs_socket_write(xsMachine *the)
{
	xsSocket xss = xsmcGetHostData(xsThis);
	int argc = xsmcArgc;
	char *msg;
	size_t msgLen;
	u16_t available, needed = 0;
	err_t err;
	unsigned char pass, arg;

	if ((NULL == xss) || !(xss->skt || xss->udp || xss->raw) || xss->writeDisabled) {
		if (0 == argc) {
			xsResult = xsInteger(0);
			return;
		}
		xsTrace("write on closed socket\n");
		return;
	}

	if (xss->udp) {
		char temp[16];
		uint8 ip[4];
		unsigned char *data;
		uint16 port = xsmcToInteger(xsArg(1));
		ip_addr_t dst;

		xsmcToStringBuffer(xsArg(0), temp, sizeof(temp));
		if (!parseAddress(temp, ip))
			xsUnknownError("invalid IP address");
		IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);

		needed = xsGetArrayBufferLength(xsArg(2));
		data = xsmcToArrayBuffer(xsArg(2));
		udp_sendto_safe(xss->udp, data, needed, &dst, port, &err);
		if (ERR_OK != err) {
			xsLog("UDP send error %d\n", err);
			xsUnknownError("UDP send failed");
		}

		modInstrumentationAdjust(NetworkBytesWritten, needed);
		return;
	}

	if (xss->raw) {
		char temp[16];
		uint8 ip[4];
		unsigned char *data;
		ip_addr_t dst = {0};
		struct pbuf *p;

		xsmcToStringBuffer(xsArg(0), temp, sizeof(temp));
		if (!parseAddress(temp, ip))
			xsUnknownError("invalid IP address");
		IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);

		needed = xsGetArrayBufferLength(xsArg(1));
		data = xsmcToArrayBuffer(xsArg(1));
		p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)needed, PBUF_RAM);
		if (!p)
			xsUnknownError("no buffer");
		c_memcpy(p->payload, data, needed);
		err = raw_sendto(xss->raw, p, &dst);
		pbuf_free(p);
		if (ERR_OK != err)
			xsUnknownError("RAW send failed");

		modInstrumentationAdjust(NetworkBytesWritten, needed);
		return;
	}

	available = tcp_sndbuf(xss->skt);
	if (0 == argc) {
		xsResult = xsInteger(available);
		return;
	}

	for (pass = 0; pass < 2; pass++ ) {
		for (arg = 0; arg < argc; arg++) {
			xsType t = xsmcTypeOf(xsArg(arg));
			unsigned char byte;

			if ((xsStringXType == t) || (xsStringType == t)) {
				msg = xsmcToString(xsArg(arg));
				msgLen = c_strlen(msg);
			}
			else if ((xsNumberType == t) || (xsIntegerType == t)) {
				msgLen = 1;
				if (pass) {
					byte = (unsigned char)xsmcToInteger(xsArg(arg));
					msg = &byte;
				}
			}
			else if (xsReferenceType == t) {
				if (xsmcIsInstanceOf(xsArg(arg), xsArrayBufferPrototype)) {
					msgLen = xsGetArrayBufferLength(xsArg(arg));
					if (pass)
						msg = xsmcToArrayBuffer(xsArg(arg));
				}
				else if (xsmcIsInstanceOf(xsArg(arg), xsTypedArrayPrototype)) {
					xsmcGet(xsResult, xsArg(arg), xsID_byteLength);
					msgLen = xsmcToInteger(xsResult);
					if (pass) {
						int byteOffset;

						xsmcGet(xsResult, xsArg(arg), xsID_byteOffset);
						byteOffset = xsmcToInteger(xsResult);

						xsmcGet(xsResult, xsArg(arg), xsID_buffer);
						msg = byteOffset + (char *)xsmcToArrayBuffer(xsResult);
					}
				}
				else if (xsmcHas(xsArg(arg), xsID_byteLength)) {	// host data
					xsmcGet(xsResult, xsArg(arg), xsID_byteLength);
					msgLen = xsmcToInteger(xsResult);
					if (pass)
						msg = xsmcGetHostData(xsArg(arg));
				}
				else
					xsUnknownError("unsupported type for write");
			}
			else
				xsUnknownError("unsupported type for write");

			if (0 == pass)
				needed += msgLen;
			else {
#if !ESP32
				uint8_t inFlash = (void *)msg >= (void *)kFlashStart;
#else
				uint8_t inFlash = false;
#endif

				if (!inFlash) {
					while (msgLen) {
						err = tcp_write_safe(xss->skt, msg, msgLen, TCP_WRITE_FLAG_COPY);
						if (ERR_OK == err)
							break;

						if (ERR_MEM != err) {
							socketSetPending(xss, kPendingError);
							return;
						}

						modDelayMilliseconds(25);
					}
				}
				else {
					// pull buffer through a temporary buffer
					while (msgLen) {
						char buffer[128];
						int use = msgLen;
						if (use > sizeof(buffer))
							use = sizeof(buffer);

						c_memcpy(buffer, msg, use);
						do {
							err = tcp_write_safe(xss->skt, buffer, use, TCP_WRITE_FLAG_COPY);
							if (ERR_OK == err)
								break;

							if (ERR_MEM != err) {
								socketSetPending(xss, kPendingError);
								return;
							}

							modDelayMilliseconds(25);
						} while (true);

						msg += use;
						msgLen -= use;
					}
				}
			}
		}

		if ((0 == pass) && (needed > available))
			xsUnknownError("can't write all data");
	}

	xss->outstandingSent += needed;

	if (xss->skt)
		socketSetPending(xss, kPendingOutput);
}

void xs_socket_suspend(xsMachine *the)
{
	xsSocket xss = xsmcGetHostData(xsThis);

	if (xsmcArgc) {
		uint8_t suspended = xsmcToBoolean(xsArg(0));
		if (!suspended) {
			modLog("resume");
			socketSetPending(xss, kPendingReceive);
			if (xss->suspendedError) {
				modLog("resume - has error");
				socketSetPending(xss, kPendingError);
			}
			if (xss->suspendedDisconnect) {
				modLog("resume - has disconnect");
				socketSetPending(xss, kPendingDisconnect);
			}
			socketSetPending(xss, kPendingReceive);
			xss->suspendedError = 0;
			xss->suspendedDisconnect = 0;
		}
		else {
			modLog("suspend");
			if (xss->pending & kPendingError) {		//@@ critical section
				modLog("suspend - pending error");
				xss->pending &= ~kPendingError;
				xss->suspendedError = 1;
			}
			if (xss->pending & kPendingDisconnect) {		//@@ critical section
				modLog("suspend - pending disconnect");
				xss->pending &= ~kPendingDisconnect;
				xss->suspendedDisconnect = 1;
			}
		}
		xss->suspended = suspended;
	}

	xsmcSetBoolean(xsResult, xss->suspended);
}

void configureSocketTCP(xsMachine *the, xsSocket xss)
{
	xsmcGet(xsVar(0), xsArg(0), xsID_keepalive);
	if (xsmcTest(xsVar(0))) {
		xsmcGet(xsVar(1), xsVar(0), xsID_enable);
		if (xsmcTest(xsVar(1))) {
			xss->skt->so_options |= SOF_KEEPALIVE;

			if (xsmcHas(xsVar(0), xsID_idle)) {
				xsmcGet(xsVar(1), xsVar(0), xsID_idle);
				xss->skt->keep_idle = xsmcToInteger(xsVar(1));
			}
			if (xsmcHas(xsVar(0), xsID_interval)) {
				xsmcGet(xsVar(1), xsVar(0), xsID_interval);
				xss->skt->keep_intvl = xsmcToInteger(xsVar(1));
			}
			if (xsmcHas(xsVar(0), xsID_count)) {
				xsmcGet(xsVar(1), xsVar(0), xsID_count);
				xss->skt->keep_cnt = xsmcToInteger(xsVar(1));
			}
		}
	}

	xsmcGet(xsVar(0), xsArg(0), xsID_noDelay);
	if (xsmcTest(xsVar(0)))
		tcp_nagle_disable(xss->skt);
}

void socketMsgConnect(xsSocket xss)
{
	xsMachine *the = xss->the;

	xsBeginHost(the);
	if (xss->skt)
		xsCall1(xss->obj, xsID_callback, xsInteger(kSocketMsgConnect));
	xsEndHost(the);
}

void socketMsgDisconnect(xsSocket xss)
{
	xsMachine *the = xss->the;

	if (xss->suspended) {
		xss->suspendedDisconnect = 1;
		return;
	}

	xsBeginHost(the);
		xsCall1(xss->obj, xsID_callback, xsInteger(kSocketMsgDisconnect));
	xsEndHost(the);
}

void socketMsgError(xsSocket xss)
{
	xsMachine *the = xss->the;

	if (xss->suspended) {
		xss->suspendedError = 1;
		return;
	}

	xsBeginHost(the);
		xsCall1(xss->obj, xsID_callback, xsInteger(kSocketMsgError));		//@@ report the error value
	xsEndHost(the);
}

void socketMsgDataReceived(xsSocket xss)
{
	xsMachine *the = xss->the;
	struct pbuf *pb;
	uint8_t i;
	ip_addr_t address;
	uint16_t port;

	if (xss->buflen && (xss->bufpos < xss->buflen))
		return;		// haven't finished reading current pbuf

	if (xss->pb) {
		if (xss->pbWalker->next) {
			xss->pbWalker = xss->pbWalker->next;

			xss->buf = xss->pbWalker->payload;
			xss->bufpos = 0;
			xss->buflen = xss->pbWalker->len;

			goto callback;
		}

		pbuf_free_safe(xss->pb);
		xss->pb = NULL;
		xss->pbWalker = NULL;
		xss->buflen = 0;
	}

	modCriticalSectionBegin();

	pb = xss->reader[0];
	if (NULL == pb) {
		modCriticalSectionEnd();		// no more to read
		return;
	}

	for (i = 0; i < kReadQueueLength - 1; i++)
		xss->reader[i] = xss->reader[i + 1];
	xss->reader[kReadQueueLength - 1] = NULL;

	if (kTCP != xss->kind) {
		address = xss->remote[0].address;
		port = xss->remote[0].port;
		c_memmove(&xss->remote[0], &xss->remote[1], (kReadQueueLength - 1) * sizeof(xsSocketUDPRemoteRecord));
	}

	modCriticalSectionEnd();

	xss->pb = pb;
	xss->pbWalker = pb;
	xss->buf = pb->payload;
	xss->bufpos = 0;
	xss->buflen = pb->len;

	if ((kTCP == xss->kind) && xss->skt)
		tcp_recved_safe(xss->skt, pb->tot_len);

callback:
	xsBeginHost(the);

	if (kTCP == xss->kind) {

#if !ESP32
		system_soft_wdt_stop();		//@@
#endif
		xsCall2(xss->obj, xsID_callback, xsInteger(kSocketMsgDataReceived), xsInteger(xss->buflen));
#if !ESP32
		system_soft_wdt_restart();		//@@
#endif
	}
	else {
		char *out;

		xsResult = xsStringBuffer(NULL, 4 * 5);
		out = xsmcToString(xsResult);
#if LWIP_IPV4 && LWIP_IPV6
		itoa(ip4_addr1(&address.u_addr.ip4), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr2(&address.u_addr.ip4), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr3(&address.u_addr.ip4), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr4(&address.u_addr.ip4), out, 10); out += strlen(out); *out = 0;
#else
		itoa(ip4_addr1(&address), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr2(&address), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr3(&address), out, 10); out += strlen(out); *out++ = '.';
		itoa(ip4_addr4(&address), out, 10); out += strlen(out); *out = 0;
#endif

		if (kUDP == xss->kind)
			xsCall4(xss->obj, xsID_callback, xsInteger(kSocketMsgDataReceived), xsInteger(xss->buflen), xsResult, xsInteger(port));
		else
			xsCall3(xss->obj, xsID_callback, xsInteger(kSocketMsgDataReceived), xsInteger(xss->buflen), xsResult);
	}

	xsEndHost(the);
}

void socketMsgDataSent(xsSocket xss)
{
	xsMachine *the = xss->the;

	xsBeginHost(the);
		xsCall2(xss->obj, xsID_callback, xsInteger(kSocketMsgDataSent), xsInteger(xss->skt ? tcp_sndbuf(xss->skt) : 0));
	xsEndHost(the);
}


#if ESP32
void didFindDNS(const char *name, const ip_addr_t *ipaddr, void *arg)
#else
void didFindDNS(const char *name, ip_addr_t *ipaddr, void *arg)
#endif
{
	xsSocket xss = arg;

	if (ipaddr)
		tcp_connect_safe(xss->skt, ipaddr, xss->port, didConnect);
	else
		socketSetPending(xss, kPendingError);
}

void didError(void *arg, err_t err)
{
	xsSocket xss = arg;

	xss->skt = NULL;		// "pcb is already freed when this callback is called"
	socketSetPending(xss, kPendingError);
}

err_t didConnect(void * arg, struct tcp_pcb * tpcb, err_t err)
{
	xsSocket xss = arg;

	if (ERR_OK != err)
		socketSetPending(xss, kPendingError);
	else
		socketSetPending(xss, kPendingConnect);

	return ERR_OK;
}

err_t didReceive(void * arg, struct tcp_pcb * pcb, struct pbuf * p, err_t err)
{
	xsSocket xss = arg;
	unsigned char i;
	struct pbuf *walker;
	uint16 offset;

#ifdef mxDebug
	if (p && fxInNetworkDebugLoop(xss->the)) {
		modLog("refuse TCP");
		return ERR_MEM;
	}
#endif

	if (xss->pending & kPendingClose)
		return ERR_OK;

	if (!p) {
		tcp_recv(xss->skt, NULL);
		tcp_sent(xss->skt, NULL);

		if (xss->suspended)
			xss->suspendedDisconnect = true;
		else {
#if ESP32
			xss->skt = NULL;			// no close on socket if disconnected.
#endif

			if (xss->reader[0] || xss->buflen)
				xss->suspendedDisconnect = true;
			else
				socketSetPending(xss, kPendingDisconnect | kPendingClose);
		}

		return ERR_OK;
	}

	modCriticalSectionBegin();
	for (i = 0; i < kReadQueueLength; i++) {
		if (NULL == xss->reader[i]) {
			xss->reader[i] = p;
			break;
		}
	}
	modCriticalSectionEnd();

	if (kReadQueueLength == i)
		return ERR_MEM;			// no space. return error so lwip will redeliver later

	modInstrumentationAdjust(NetworkBytesRead, p->tot_len);

	if (!xss->suspended)
		socketSetPending(xss, err ? kPendingError : kPendingReceive);
	else {
		if (err)
			xss->suspendedError = true;
	}

	return ERR_OK;
}

err_t didSend(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	xsSocket xss = arg;

	modInstrumentationAdjust(NetworkBytesWritten, len);

	xss->outstandingSent -= len;
	if (0 == xss->outstandingSent)
		socketSetPending(xss, kPendingSent);

	return ERR_OK;
}

void didReceiveUDP(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *remoteAddr, u16_t remotePort)
{
	xsSocket xss = arg;
	unsigned char i;
	struct pbuf *toFree = NULL;

#ifdef mxDebug
	if (fxInNetworkDebugLoop(xss->the)) {
		pbuf_free(p);
		return;
	}
#endif

	modCriticalSectionBegin();

	for (i = 0; i < kReadQueueLength; i++) {
		if (NULL == xss->reader[i])
			break;
	}

	if (kReadQueueLength == i) {		// all full. make room by tossing earliest entry
		toFree = xss->reader[0];
		for (i = 0; i < kReadQueueLength - 1; i++) {
			xss->reader[i] = xss->reader[i + 1];
			xss->remote[i] = xss->remote[i + 1];
		}
		i = kReadQueueLength - 1;
	}

	xss->reader[i] = p;
	xss->remote[i].port = remotePort;
	xss->remote[i].address = *remoteAddr;

	modInstrumentationAdjust(NetworkBytesRead, p->tot_len);
	modCriticalSectionEnd();

	if (toFree)
		pbuf_free(toFree);		// not pbuf_free_safe, because we are in lwip task

	socketSetPending(xss, kPendingReceive);
}

#if ESP32
u8_t didReceiveRAW(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
#else
u8_t didReceiveRAW(void *arg, struct raw_pcb *pcb, struct pbuf *p, ip_addr_t *addr)
#endif
{
	didReceiveUDP(arg, (struct udp_pcb *)pcb, p, addr, 0);
	return 1;
}

static uint8 parseAddress(char *address, uint8_t *ip)
{
	char *p = address;
	int i;
	for (i = 0; i < 3; i++) {
		char *separator = c_strchr(p, (i < 3) ? '.' : 0);
		if (!separator)
			return 0;
		*separator = 0;
		ip[i] = (unsigned char)atoi(p);
		p = separator + 1;
	}
	ip[3] = (unsigned char)atoi(p);

	return 1;
}

static err_t didAccept(void * arg, struct tcp_pcb * newpcb, err_t err);

// to accept an incoming connection: let incoming = new Socket({listener});
void xs_listener(xsMachine *the)
{
	xsListener xsl;
	uint16 port = 0;
	err_t err;

	// allocate listener
	xsl = c_calloc(1, sizeof(xsListenerRecord));
	if (!xsl)
		xsUnknownError("out of memory");

	xsmcSetHostData(xsThis, xsl);

	modInstrumentationAdjust(NetworkSockets, 1);

	if (xsmcHas(xsArg(0), xsID_port)) {
		xsmcVars(1);
		xsmcGet(xsVar(0), xsArg(0), xsID_port);
		port = (uint16)xsmcToInteger(xsVar(0));
	}

	xsl->obj = xsThis;
	xsl->the = the;
	socketUpUseCount(the, (xsSocket)xsl);
	xsRemember(xsl->obj);

	xsl->kind = kTCPListener;
	xsl->constructed = true;

	xsl->skt = tcp_new_safe();
	if (!xsl->skt)
		xsUnknownError("socket allocation failed");

	ip_addr_t address = *(IP_ADDR_ANY);
	if (xsmcHas(xsArg(0), xsID_address)) {
		char temp[DNS_MAX_NAME_LENGTH];
		uint8_t ip[4];
		xsmcGet(xsVar(0), xsArg(0), xsID_address);
		if (xsmcTest(xsVar(0))) {
			xsmcToStringBuffer(xsVar(0), temp, sizeof(temp));
			if (!parseAddress(temp, ip))
				xsUnknownError("invalid IP address");
			IP_ADDR4(&address, ip[0], ip[1], ip[2], ip[3]);
		}
	}

	xsl->skt->so_options |= SOF_REUSEADDR;
	err = tcp_bind_safe(xsl->skt, &address, port);
	if (err)
		xsUnknownError("socket bind");

	xsl->skt = tcp_listen_safe(xsl->skt);

	tcp_arg(xsl->skt, xsl);

	tcp_accept(xsl->skt, didAccept);
}

void xs_listener_destructor(void *data)
{
	xsListener xsl = data;
	uint8 i;

	if (!xsl) return;

	if (xsl->skt) {
		tcp_accept(xsl->skt, NULL);
		tcp_close(xsl->skt);
	}

	for (i = 0; i < kListenerPendingSockets; i++)
		xs_socket_destructor(xsl->accept[i]);

	c_free(xsl);

	modInstrumentationAdjust(NetworkSockets, -1);
}

void xs_listener_close(xsMachine *the)
{
	xsListener xsl = xsmcGetHostData(xsThis);

	if ((NULL == xsl) || (xsl->pending & kPendingClose))
		xsUnknownError("close on closed listener");

	socketSetPending((xsSocket)xsl, kPendingClose);
}

static void listenerMsgNew(xsListener xsl)
{
	xsMachine *the = xsl->the;
	uint8 i;

	// service all incoming sockets currently in the list
	for (i = 0; i < kListenerPendingSockets; i++) {
		if (NULL == xsl->accept[i])
			continue;

		xsBeginHost(the);
		xsCall1(xsl->obj, xsID_callback, xsInteger(kListenerMsgConnect));
		xsEndHost(the);
	}
}

static err_t didReceiveWait(void * arg, struct tcp_pcb * pcb, struct pbuf * p, err_t err)
{
	return ERR_MEM;
}

err_t didAccept(void * arg, struct tcp_pcb * newpcb, err_t err)
{
	xsListener xsl = arg;
	xsSocket xss;
	uint8 i;

	tcp_accepted(xsl->skt);

#ifdef mxDebug
	if (fxInNetworkDebugLoop(xsl->the)) {
		modLog("refuse incoming connection while in debugger");
		return ERR_ABRT;		// lwip will close
	}
#endif

	xss = c_calloc(1, sizeof(xsSocketRecord) - sizeof(xsSocketUDPRemoteRecord));
	if (!xss)
		return ERR_ABRT;		// lwip will close

	xss->the = xsl->the;
	xss->skt = newpcb;
	xss->useCount = 1;
	xss->kind = kTCP;

	modCriticalSectionBegin();
	for (i = 0; i < kListenerPendingSockets; i++) {
		if (NULL == xsl->accept[i]) {
			xsl->accept[i] = xss;
			break;
		}
	}
	modCriticalSectionEnd();

	if (kListenerPendingSockets == i) {
		modLog("tcp accept queue full");
		c_free(xss);
		return ERR_ABRT;		// lwip will close
	}

	tcp_arg(xss->skt, xss);
	tcp_recv(xss->skt, didReceiveWait);
	tcp_sent(xss->skt, didSend);
	tcp_err(xss->skt, didError);

	socketSetPending((xsSocket)xsl, kPendingAcceptListener);

	modInstrumentationAdjust(NetworkSockets, 1);

	return ERR_OK;
}

void socketSetPending(xsSocket xss, uint8_t pending)
{
	uint8_t doSchedule;

	modCriticalSectionBegin();

	if (((xss->pending & pending) == pending) || (xss->pending & kPendingClose)) {
		modCriticalSectionEnd();
		return;
	}

	doSchedule = 0 == xss->pending;
	xss->pending |= pending;

	if (xss->pending & (kPendingError | kPendingDisconnect))
		xss->writeDisabled = true;

	if (doSchedule && (xss->constructed || (pending & kPendingAcceptListener))) {
		socketUpUseCount(xss->the, xss);
		modCriticalSectionEnd();

		modMessagePostToMachine(xss->the, NULL, 0, socketClearPending, xss);
	}
	else
		modCriticalSectionEnd();
}

void socketClearPending(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	xsSocket xss = refcon;
	uint8_t pending;

	modCriticalSectionBegin();

	pending = xss->pending;
	xss->pending &= kPendingClose;		// don't clear close flag

	modCriticalSectionEnd();

	if (!pending) {
		modLog("socketClearPending called - NOTHING PENDING?");
		goto done;		// return or done...
	}

	if ((pending & kPendingReceive) && !(xss->pending & kPendingClose))
		socketMsgDataReceived(xss);

	if ((pending & kPendingSent) && !(xss->pending & kPendingClose))
		socketMsgDataSent(xss);

	if ((pending & kPendingOutput) && !(xss->pending & kPendingClose))
		tcp_output_safe(xss->skt);

	if ((pending & kPendingConnect) && !(xss->pending & kPendingClose))
		socketMsgConnect(xss);

	if ((pending & kPendingDisconnect) && !(xss->pending & kPendingClose))
		socketMsgDisconnect(xss);

	if ((pending & kPendingError) && !(xss->pending & kPendingClose))
		socketMsgError(xss);

	if ((pending & kPendingAcceptListener) && !(xss->pending & kPendingClose))
		listenerMsgNew((xsListener)xss);

	if (pending & kPendingClose) {
		if (socketDownUseCount(xss->the, xss))
			return;
	}

done:
	socketDownUseCount(xss->the, xss);
}

void *modSocketGetLWIP(xsMachine *the, xsSlot *slot)
{
	xsSocket xss = xsmcGetHostData(*slot);
	struct tcp_pcb *skt = xss->skt;
	if (skt) {
		socketSetPending(xss, kPendingDisconnect | kPendingClose);
		xss->skt = NULL;
		tcp_recv(skt, NULL);
		tcp_sent(skt, NULL);
		tcp_err(skt, NULL);
	}
	return skt;
}

