#ifndef NL_PLAT_SOCKET_NETWORK_H
#define NL_PLAT_SOCKET_NETWORK_H

void SocketNetworkInitializeMemory();
// (1.3) MSC_NET_POOLS_AT_BOOT=1: the two network memory pools (socket 200 KB, DWC 700 KB) are made at boot as in the retail game; default 0: when first asked for (never, in this port)
bool NpNetPoolsAtBoot();
void SocketNetworkStartup();
void SocketNetworkShutdown();
bool SocketNetworkIsStarted();
int SocketNetworkGetLastError();
void SocketNetworkStartupAsync();
bool SocketNetworkIsStartupComplete();

#endif // NL_PLAT_SOCKET_NETWORK_H
