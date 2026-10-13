#ifndef OBY_NET_H
#define OBY_NET_H

/* Register the TCP socket natives (net_connect, net_listen, net_accept, ...)
   with the native registry. POSIX only; a Windows build needs winsock. */
void net_register_natives(void);

#endif // OBY_NET_H
