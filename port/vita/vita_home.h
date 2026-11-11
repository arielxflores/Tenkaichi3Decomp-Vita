/* Where the program keeps its files, and whose program it is: the BT3 build, or the Budokai Tenkaichi 4 build
   (build.py BT3_BT4: BT4decomp's game code, started from the BT3 package as app0:bt4.bin) */
#ifndef VITA_HOME_H
#define VITA_HOME_H

#ifdef BT3_BT4
#define VITA_HOME "ux0:data/bt3/bt4"          /* its saves, settings and log; the disc's files in it as they are */
#define VITA_DISC_DEFAULT VITA_HOME
#define VITA_PROGRAM "SLUS_219.78"
#define VITA_GAME_NAME "Budokai Tenkaichi 4 (BT4decomp)"
#define VITA_RELEASE_NAME "Team BT4's Budokai Tenkaichi 4 B14 Rev2"
#else
#define VITA_HOME "ux0:data/bt3"
#define VITA_DISC_DEFAULT VITA_HOME "/disc"
#define VITA_PROGRAM "SLUS_216.78"
#define VITA_GAME_NAME "Tenkaichi3Decomp"
#define VITA_RELEASE_NAME "USA release (SLUS-21678)"
#endif

#endif
