/* Songs added to the music select (z3xox's PC version, docs/port/adding_songs.md there; on the Vita the folder is
   ux0:data/bt3/songs, port/vita/vita_extras.c). A song is an ADX file in that folder (an MP3 there is made into one
   the first time the game starts with it); the port serves it for a file id of its own and the menus list it. */
#ifndef PORT_PLAT_SONGS_H
#define PORT_PLAT_SONGS_H

/* The music list holds offsets into the BGM file range: entry value `n` plays file 0x10B16 + n. Offsets 0x00..0x13
   are the disc's 20 tracks, 0x18 is the game's "random" and 0x19 its "locked" marker; the disc's own raw list also
   carries 0x14..0x17, which the menu drops. Added tracks take 0x1A onward, so they never touch a disc entry. */
#define PORT_SONG_FIRST_OFFSET 0x1A
#define PORT_SONG_BGM_FIRST 0x10B16 /* Bgm_Play id of music-list offset 0 */
#define PORT_SONG_MAX 40            /* the menu's list (72 places of ours) less the disc's songs and "random" */
/* An added song's file: past the end of the second archive (65,201 files), not 0x10B16 + its entry (0x10B30.. are
   disc files: silent ones, then the announcer's first lines). */
#define PORT_SONG_FILE_INDEX 73000
#define PORT_BGM_FILE(n) \
    ((n) >= PORT_SONG_FIRST_OFFSET && (n) < PORT_SONG_FIRST_OFFSET + PORT_SONG_MAX ? 0xD48 + PORT_SONG_FILE_INDEX + ((n) - PORT_SONG_FIRST_OFFSET) \
                                                                                    : PORT_SONG_BGM_FIRST + (n))

void PortSongs_Init(void);
void PortSongs_Refresh(void);       /* the select screens call it as they set up their music list */
int PortSongs_Count(void);
int PortSongs_Offset(int index);    /* the music-list entry value of the index-th added track */
const char *PortSongs_Name(int index);
int PortSongs_IsFile(const char *path); /* the file of an added song (played round and round) */

/* The game reads these. */
extern char gPortSongNames[PORT_SONG_MAX][64];
extern int gPortSongCount;
extern int gPortSongOffsets[PORT_SONG_MAX];

#endif
