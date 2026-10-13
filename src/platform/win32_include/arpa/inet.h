/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: <arpa/inet.h> for MinGW (Windows only; build-windows.sh puts src/platform/win32_include
 * on the include path). Winsock declares the same byte-order and address-conversion functions
 * (htons/htonl/ntohs/ntohl, inet_pton/inet_ntop, AF_INET); the sacredfire.exe link has ws2_32, and
 * the GPU library's boost::asio has called WSAStartup before the game runs. */
#ifndef REFUELED_WIN32_ARPA_INET_H
#define REFUELED_WIN32_ARPA_INET_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
