/* adb over Wi-Fi: closed on an installed Echo, opened for a while on request (scripts/device/lockdown.sh, adb_gate). */
#ifndef ADBWIFI_H
#define ADBWIFI_H
void adbwifi_start(void (*changed)(void));  /* watcher thread; changed(): adbwifi_open() changed, called without locks */
void adbwifi_ask(int on);                   /* any thread: ask root's firewall watcher to open it (30 min, for the network) or close it */
void adbwifi_ask_from(const char *ip);      /* the same, opened for this one IPv4 address only (lockdown.sh checks it again) */
int  adbwifi_open(void);                    /* 1 while open, or asked for and not answered yet */
int  adbwifi_granted(void);                 /* the request has been taken and the port is open */
#endif
