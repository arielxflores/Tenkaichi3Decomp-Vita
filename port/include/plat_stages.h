/* Stages added from outside the disc (z3xox's PC version, docs/port/adding_stages.md there; on the Vita the folder
   is ux0:data/bt3/stages, port/vita/vita_extras.c). A stage is a .unk file, the stage model BT3 modders release:
   each one becomes the next stage id from PORT_STAGE_FIRST on, the port serves its model for a file id of its own
   and the menus list it. */
#ifndef PORT_PLAT_STAGES_H
#define PORT_PLAT_STAGES_H

#define PORT_STAGE_FIRST 0x24 /* first added stage id: the disc's stages are 0x00..0x23 */
#define PORT_STAGE_MAX 26     /* ids 0x24..0x3D (the stage-list bit field is 64 bits, 0x3E / 0x3F are the markers) */

void PortStages_Init(void);
int PortStages_Count(void);
const char *PortStages_Name(int index); /* the display name of the index-th added stage */
/* The disc stage (0..0x23) an added stage (id 0x24 on) was made from, which its model says (its parameter block's
   stage number): its sounds are that stage's (the stage's ambience plays them). 0 when the model does not say. */
int Port_AddedStageBase(int stage);
/* The same for its icon and picture in the menus: "Random"'s (0x23, a question mark) when the model does not say. */
int Port_AddedStageLook(int stage);

/* The game reads these. */
extern char gPortStageNames[PORT_STAGE_MAX][64];
extern int gPortExtraStageCount;
extern int gPortExtraStages[PORT_STAGE_MAX];

/* The names of added stages and songs in the game's own lettering (port/vita/vita_names.c): the name written into
   a row of the stage select's name pictures that the disc's entries do not show at that moment. kind 0 a stage,
   1 a song; idx the index of the added one. 1 with the picture (sheet) and its row to show; 0: drawn as text. */
int Port_NameRow(int kind, int idx, int *sheet, int *row);
/* A disc entry about to be shown (kind 0: stage id; 1: music-list value): its own row back if an added name had it. */
void Port_NameRowDisc(int kind, int id);

#endif
