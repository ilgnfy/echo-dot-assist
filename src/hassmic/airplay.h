/* AirPlay 2 receiver: shairport-sync (a static build, tools/airplay) as hassmic's child, its audio through a FIFO into the music stream.
 * One more music source beside Sendspin and Bluetooth (core_music: the newest wins).  Off by default (setting "airplay"). */
#ifndef AIRPLAY_H
#define AIRPLAY_H
void airplay_init(void);                /* after the name is made: the supervisor and the FIFO reader threads */
int  airplay_present(void);             /* the receiver is installed next to hassmic (shairport-sync) */
int  airplay_enable(int set);           /* -1: ask; 0/1: set (the setting "airplay", saved by settings.c) */
int  airplay_delay(int set);            /* trim in ms on top of the latency worked out from the mixer queue, the FIFO and the output; INT_MIN: ask */
int  airplay_playing(void);             /* audio is coming from a sender */
void airplay_yield(void);               /* another music source started: drop the sender (it shows "disconnected") */
#endif
