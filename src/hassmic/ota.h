/* Push updates: receive a signed bundle over TCP and hand it to the root-side installer (scripts/system/main.sh). */
#ifndef OTA_H
#define OTA_H
int ota_start(int port);        /* listener thread; does nothing useful without /system/hassmic/update.pub.  Also
                                   opens adb over Wi-Fi for whoever holds the update key (ota.c) */
void ota_healthy(void);         /* the self test passed: root makes the update that runs the factory copy (main.sh) */
#endif
