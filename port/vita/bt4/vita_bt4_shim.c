/*
 * The Budokai Tenkaichi 4 program (build.py BT3_BT4): stand-ins for the services of BT4decomp's PC platform layer that
 * its game code calls and the Vita does not have. Online play, Tag Team rooms, lobbies and ranked matches, the PC's
 * widescreen menu layout, spectating, Lua camera mods, the PC's text overlay and translations, custom stages and songs:
 * each answers as BT4decomp does when that feature is not in use (nothing open, nothing added, the game's own choice).
 * The main menu does not list "Tenkaichi Budokai Online" on the Vita (src/menu/menu_a.c), the only way into them.
 * Made from the declarations of BT4decomp 92b87f4; port/vita/bt4/vita_bt4.c has the services the Vita does provide.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <psp2/kernel/processmgr.h>

void MenuText_BootLanguageChosen(void) {  }
void MenuText_RegisterGlyphs(int tbp, int cbp, const void *glyphs, int count, int lineHeight) { (void)tbp; (void)cbp; (void)glyphs; (void)count; (void)lineHeight; }
void MenuTranslation_SaveChoice(const char *locale) { (void)locale; }
void PortA11y_FontText(int kind, int arg, int flags) { (void)kind; (void)arg; (void)flags; }
void PortA11y_Say(const char *text, int flags) { (void)text; (void)flags; }
int PortHud_Hidden(void) { return 0; }
void PortHud_Toggle(void) {  }
void PortOnline_Raw(int on) { (void)on; }
const char *PortOnline_Text(const char *text) { return text; }
void PortQuit_Draw(int vsyncs) { (void)vsyncs; }
void PortSongs_Refresh(void) {  }
int PortSpecMenu_Frame(int vsyncs) { (void)vsyncs; return 0; }
unsigned short *PortSpecMenu_Text(int k) { static unsigned short none[1]; (void)k; return none; }
int PortSpectator_Camera(const float *native_eye, const float *native_rot, const float *targets, int count, float *eye, float *rot) { (void)native_eye; (void)native_rot; (void)targets; (void)count; (void)eye; (void)rot; return 0; }
void PortSpectator_EndBattle(void) {  }
int PortSpectator_Mode(void) { return 0; }
void PortSpectator_ResolveEye(const float *eye) { (void)eye; }
void PortSpectator_SetInputActive(int active) { (void)active; }
void PortTbo_CommunityBounds(int active, float left, float top, float right, float bottom) { (void)active; (void)left; (void)top; (void)right; (void)bottom; }
int PortTbo_CommunityTake(void) { return 0; }
void PortTbo_Copy(const char *text) { (void)text; }
void PortTbo_OpenCommunity(void) {  }
void PortTbo_Paste(void) {  }
void PortTbo_PasteText(void) {  }
unsigned PortTbo_TextGeneration(void) { return 0; }
int PortTbo_TextId(const char *text, int kind) { (void)text; (void)kind; return 0; }
int PortTbo_TextIdLiteral(const char *text, int kind) { (void)text; (void)kind; return 0; }
int PortTbo_TextInfo(int id, int what) { (void)id; (void)what; return 0; }
int PortTbo_TextMeasure(const char *text, int kind) { (void)text; (void)kind; return 0; }
int PortTbo_TextMeasureLiteral(const char *text, int kind) { (void)text; (void)kind; return 0; }
void PortTbo_TextMode(int on) { (void)on; }
int PortTbo_TextRead(void) { return 0; }
void *Port_ActorAdx(int actor, int voice) { (void)actor; (void)voice; return NULL; }
void Port_ActorAdxPauseAll(int paused) { (void)paused; }
void Port_ActorAdxStop(int actor, int voice) { (void)actor; (void)voice; }
void Port_ActorAdxStopAll(void) {  }
int Port_AdxStoryProbe(int channel, int partition, int file) { (void)channel; (void)partition; (void)file; return 0; }
void Port_BoundaryBegin(void) {  }
int Port_BoundaryDrawing(void) { return 0; }
void Port_BoundaryEnd(void) {  }
void Port_ClashLog(int player, const char *what, int fromInput, int total, int frame) { (void)player; (void)what; (void)fromInput; (void)total; (void)frame; }
int Port_DataFingerprint(unsigned long long *out) { if (out != NULL) { *out = 0; } return 0; }
int Port_DataFingerprintNeed(void) { return 0; }
int Port_DataFingerprintOriginal(void) { return 0; }
const char *Port_DataFingerprintText(void) { return ""; }
void Port_DevRulesEncode(unsigned char *out) { (void)out; }
/* (Port_FxOff: vita_main.c, BT3's and BT4's alike: bt3.txt weather=, moveblur=) */
void Port_HudFeed(int kind, int side, int a, int b) { (void)kind; (void)side; (void)a; (void)b; }
int Port_HudLayout(void) { return 0; }
int Port_HudSwap(void) { return 0; }
void Port_LobbyBackground(int on) { (void)on; }
void Port_LobbyCancel(void) {  }
const char *Port_LobbyCode(void) { return ""; }
int Port_LobbyCreate(int byCode, int port, int battle, int time, int speed, int rollback, int delay) { (void)byCode; (void)port; (void)battle; (void)time; (void)speed; (void)rollback; (void)delay; return 0; }
void Port_LobbyDo(int what, int a, int b) { (void)what; (void)a; (void)b; }
const char *Port_LobbyError(void) { return ""; }
int Port_LobbyInBackground(void) { return 0; }
int Port_LobbyJoin(const char *address, int port) { (void)address; (void)port; return 0; }
void Port_LobbyLaunch(void) {  }
int Port_LobbyOther(void) { return 0; }
int Port_LobbyPoll(void) { return 0; }
int Port_LobbyQueueWaiting(int queue) { (void)queue; return -1; }
void Port_LobbyQueuesAsk(void) {  }
void Port_LobbyQuickArea(const char *area) { (void)area; }
int Port_LobbyRelease(int *min, int *latest) { (void)min; (void)latest; return 0; }
void Port_LobbyReviewFixture(int entrants) { (void)entrants; }
const char *Port_LobbyRivalRegion(void) { return ""; }
void Port_LobbySay(const char *text) { (void)text; }
int Port_LobbyStartCode(int host, const char *code) { (void)host; (void)code; return 0; }
int Port_LobbyStartQuick(int queue) { (void)queue; return 0; }
int Port_LobbyView(void *out) { (void)out; return 0; }
int Port_LobbyWantsScreen(void) { return 0; }
int Port_LobbyWiden(void) { return 0; }
int Port_MenuClipMoving(const char *name, int x, int top) { (void)name; (void)x; (void)top; return 0; }
int Port_MenuClipPart(const char *name, int x, int top) { (void)name; (void)x; (void)top; return -1; /* (none: the widescreen menus of the PC are off) */ }
int Port_MenuClipPlace(int x0, int x1) { (void)x0; (void)x1; return 0; }
void Port_MenuDuelSettings(int on) { (void)on; }
void Port_MenuFill(int on) { (void)on; }
int Port_MenuFooter(int side) { (void)side; return -1; }
int Port_MenuFree(int on) { (void)on; return 0; }
float Port_MenuLabelScale(const char *name) { (void)name; return 1.0f; }
int Port_MenuMessageX(int side) { (void)side; return 0; }
void Port_MenuMovie(int index) { (void)index; }
void Port_MenuPage(int screen) { (void)screen; }
int Port_MenuPart(int part) { (void)part; return 0; }
int Port_MenuPartNow(void) { return -1; }
float Port_MenuReadableScale(void) { return 1.0f; }
void Port_MenuScene3D(int on) { (void)on; }
float Port_MenuTextNarrow(void) { return 1.0f; }
int Port_MenusWide(void) { return 0; }
int Port_ModsCamCollide(int side) { (void)side; return 0; }
int Port_ModsCamZoom(int side) { (void)side; return 1000; }
void Port_ModsCamera(int side, int objId, int pos, int rot, int paused) { (void)side; (void)objId; (void)pos; (void)rot; (void)paused; }
void Port_NameFontSheets(int stage, int song, int songLit) { (void)stage; (void)song; (void)songLit; }
void Port_NaturalFightCamera(int objId, int pos, int rot) { (void)objId; (void)pos; (void)rot; }
int Port_NetActive(void) { return 0; }
int Port_NetDevRulesPad(int player, unsigned char *out) { (void)player; (void)out; return 0; }
int Port_NetFixedStart(void) { return 0; }
const char *Port_NetLanAddress(void) { return ""; }
void Port_NetLeave(void) {  }
int Port_NetLobbyOwnerSide(void) { return 0; }
int Port_NetMe(void) { return 0; }
void Port_NetMenuOpen(void) {  }
const char *Port_NetName(int player) { (void)player; return ""; }
const char *Port_NetNameGet(void) { return ""; }
void Port_NetNameSave(const char *name) { (void)name; }
void Port_NetNameSet(const char *name) { (void)name; }
void Port_NetOptions(int rollback, int delay) { (void)rollback; (void)delay; }
void Port_NetRulesSet(int type, int dp, int time) { (void)type; (void)dp; (void)time; }
int Port_NetSession(void) { return 0; }
int Port_NetSessionsEnded(void) { return 0; }
int Port_NetSideOptions(int *camera, int *shake) { (void)camera; (void)shake; return 0; }
int Port_NetSpecStatus(int *out) { (void)out; return 0; }
int Port_NetSpecView(void) { return 0; }
int Port_NetSpectating(void) { return 0; }
void Port_NetSpeedSet(int percent) { (void)percent; }
int Port_NetSpeedStandard(void) { return 0; }
int Port_NetView(void) { return -1; } /* (no match: -1, "no view of its own"; 0 made a split-screen fight's one-view moments draw only player 1) */
const char *Port_OnlineBlocked(void) { return "Online play is not available on the PS Vita."; }
void Port_OnlineNicknameBegin(void) {  }
int Port_OnlineNicknameSubmit(const char *name) { (void)name; return 0; }
int Port_OnlineNicknameView(char *name) { (void)name; return 0; }
int Port_OnlinePresence(int active) { (void)active; return 0; }
int Port_PageAspectMilli(void) { return 1333; /* (the menus and the loading screens as the PS2 lays them out: the renderer widens them) */ }
int Port_RankedActive(void) { return 0; }
void Port_RankedPage(int side) { (void)side; }
void Port_RankedRefresh(void) {  }
void Port_RankedSearch(int on) { (void)on; }
int Port_RankedTopTen(void *out) { (void)out; return 0; }
int Port_RankedView(void *out) { (void)out; return 0; }
int Port_RollCan(void) { return 0; }
int Port_RumbleTest(void) { return 0; }
void Port_SeActorFile(int actor, int file) { (void)actor; (void)file; }
int Port_SeActorLoop(int actor, int id) { (void)actor; (void)id; return 0; }
void Port_SeBind(int handle, int actor) { (void)handle; (void)actor; }
void Port_SettingText(const char *name, const char *def, char *out, int size) { (void)name; snprintf(out, (size_t)size, "%s", def != NULL ? def : ""); }
void Port_SettingTextSave(const char *name, const char *value) { (void)name; (void)value; }
void Port_SongDebug(int state, int idx, int px, int py, int cx, int cy) { (void)state; (void)idx; (void)px; (void)py; (void)cx; (void)cy; }
int Port_SpecLineShown(void) { return 0; }
int Port_SpecialMoveCameras(int change) { (void)change; return 1; /* (the game's own cameras for special moves, as BT4decomp by default) */ }
unsigned Port_StageArt(int stage, int kind) { (void)stage; (void)kind; return 0; }
int Port_StageBufSize(void) { return 0x6CB800; /* (the game's own: no stages added from outside the disc) */ }
int Port_TagAllArmed(void) { return 0; }
int Port_TagAllEnded(void) { return 0; }
int Port_TagAllLoaded(void) { return 0; }
void Port_TagArena(int stage, int bgm, int attacks) { (void)stage; (void)bgm; (void)attacks; }
void Port_TagBattleRenderSet(int active) { (void)active; }
int Port_TagBgm(void) { return 0; }
int Port_TagCPU(int seat) { (void)seat; return 0; }
void Port_TagCPUOptions(int fill, int difficulty) { (void)fill; (void)difficulty; }
int Port_TagCanStart(void) { return 0; }
int Port_TagCancelSelect(void) { return 0; }
int Port_TagCapacity(void) { return 0; }
void Port_TagCapture(void) {  }
int Port_TagChatCount(void) { return 0; }
const char *Port_TagChatLine(int i) { (void)i; return ""; }
void Port_TagChatSend(const char *text) { (void)text; }
void Port_TagClose(void) {  }
const char *Port_TagCode(void) { return ""; }
void Port_TagCommand(int command, int a, int b) { (void)command; (void)a; (void)b; }
int Port_TagConfirmed(int seat) { (void)seat; return 0; }
int Port_TagConnect(const char *server) { (void)server; return 0; }
int Port_TagCopyCode(void) { return 0; }
int Port_TagCount(void) { return 0; }
int Port_TagDataFingerprint(unsigned long long *out) { if (out != NULL) { *out = 0; } return 0; }
int Port_TagDelay(void) { return 0; }
int Port_TagDifficulty(int seat) { (void)seat; return 0; }
int Port_TagDraftPending(void) { return 0; }
int Port_TagDraftSeat(void) { return -1; }
const char *Port_TagEndpoint(void) { return ""; }
const char *Port_TagError(void) { return ""; }
int Port_TagFreeForAll(void) { return 0; }
void Port_TagHash(unsigned frame, unsigned hash) { (void)frame; (void)hash; }
void Port_TagHashActor(int actor, unsigned x, unsigned y, unsigned z, unsigned hp, unsigned ki, unsigned action, unsigned target, unsigned team, unsigned chara, unsigned extra) { (void)actor; (void)x; (void)y; (void)z; (void)hp; (void)ki; (void)action; (void)target; (void)team; (void)chara; (void)extra; }
int Port_TagHost(void) { return 0; }
void Port_TagHudTrace(int actor, int front, int kind, const float *basis, const float *own, const float *pos, const float *head, const float *scr, const float *marker, const float *radar) { (void)actor; (void)front; (void)kind; (void)basis; (void)own; (void)pos; (void)head; (void)scr; (void)marker; (void)radar; }
void Port_TagHudView(const float *v, const float *p, const float *nearZ) { (void)v; (void)p; (void)nearZ; }
int Port_TagHumans(void) { return 0; }
void Port_TagInitialTeams(int left, int right) { (void)left; (void)right; }
void Port_TagJoin(const char *code, const char *name, unsigned long long content) { (void)code; (void)name; (void)content; }
int Port_TagJoinCode(const char *code) { (void)code; return 0; }
unsigned Port_TagMenuMillis(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }
void Port_TagMenuSet(int a, int b) { (void)a; (void)b; }
void Port_TagMenuTestAdvance(unsigned ms) { (void)ms; }
void Port_TagModeOptions(int free_for_all, int quick) { (void)free_for_all; (void)quick; }
int Port_TagMovePlayer(int from, int to) { (void)from; (void)to; return 0; }
int Port_TagNetwork(void) { return 0; }
void Port_TagOpen(int size, int limit, int delay, const char *name, unsigned long long content) { (void)size; (void)limit; (void)delay; (void)name; (void)content; }
int Port_TagPhase(void) { return 0; }
int Port_TagPlayerChara(int seat) { (void)seat; return -1; }
int Port_TagPlayerCostume(int seat) { (void)seat; return 0; }
const char *Port_TagPlayerName(int seat) { (void)seat; return ""; }
void Port_TagPoll(void) {  }
int Port_TagQueueWaiting(int queue) { (void)queue; return -1; }
int Port_TagReselect(void) { return 0; }
int Port_TagResultChoice(void) { return 0; }
int Port_TagSeat(void) { return -1; }
unsigned Port_TagSeed(void) { return 0; }
int Port_TagSendInput(unsigned frame, const unsigned char *pad) { (void)frame; (void)pad; return 0; }
int Port_TagSlot(int seat, int cpu, int difficulty) { (void)seat; (void)cpu; (void)difficulty; return 0; }
int Port_TagSlots(void) { return 0; }
int Port_TagStage(void) { return 0; }
int Port_TagTakeFrame(unsigned char *out) { (void)out; return 0; }
int Port_TagTeam(int seat) { return seat & 1; }
int Port_TagTeamCount(int team) { (void)team; return 0; }
int Port_TagTeamIndex(int seat) { (void)seat; return 0; }
int Port_TagTeamSlots(int team) { (void)team; return 0; }
int Port_TagTeams(int left, int right) { (void)left; (void)right; return 0; }
int Port_TagTime(void) { return 0; }
void Port_TagUiCapture(int index) { (void)index; }
void Port_TagWait(int phase, int menu, int cursor, int show) { (void)phase; (void)menu; (void)cursor; (void)show; }
void Port_TagWaitClear(void) {  }
void Port_TagWaitResumed(void) {  }
void Port_TagYield(void) {  }
int Port_TestBgm(void) { return -1; }
int Port_TestScreen(void) { return -1; }
int Port_TestStage(void) { return -1; }
int Port_TrLine(const char *original, char *out, int size) { (void)original; (void)out; (void)size; return 0; }
void Port_UiSaving(int state) { (void)state; }
int Port_UpdateMenuScreen(void) { return 0; }
void Port_WallMesh(const void *key, int second, int culled, const float *screen, const float *cam, const float *vtx, int quadCount, const void *quads, unsigned long long tex0) { (void)key; (void)second; (void)culled; (void)screen; (void)cam; (void)vtx; (void)quadCount; (void)quads; (void)tex0; }
double Port_pow(double a, double b) { return pow(a, b); }
const unsigned short *UiText_TranslateLine(const unsigned *table, int line, const unsigned short *original) { (void)table; (void)line; return original; }
const unsigned short *UiText_TranslateString(const unsigned short *original) { return original; }
void Ui_LanguageChosen(const char *locale) { (void)locale; }
int Ui_QuitAsk(void) { return 0; }
int gPortExtraStageCount;
int gPortExtraStages[26];
unsigned gPortLiveBlanks;
int gPortOverlayOpen;
int gPortReplaceNew[16];
int gPortReplaceOld[16];
int gPortResim;
int gPortSongCount;
char gPortSongNames[40][64];
int gPortSongOffsets[40];
char gPortStageNames[26][64];
int gPortStageReplaceCount;
int gPortTagUiCaptureRequest;
volatile int gUiNameH;
volatile int gUiNameIdx = -1;
volatile int gUiNameReady;
volatile int gUiNameW;
volatile int gUiNameX;
volatile int gUiNameY;
volatile int gUiSongH;
volatile int gUiSongIdx = -1;
volatile int gUiSongLit;
volatile int gUiSongReady;
volatile int gUiSongW;
volatile int gUiSongX;
volatile int gUiSongY;
