/* what vita_net.c refers to and the test does not need: nothing happens, 0 or "not there" */
#define Z(n) int n() { return 0; }
#define F(n) int n() { return -1; }
Z(sceAppMgrLoadExec) Z(sceAppUtilInit) F(sceAppUtilSystemParamGetString) Z(sceCommonDialogSetConfigParam) Z(sceCommonDialogUpdate)
Z(sceDisplayGetFrameBuf) Z(sceDisplaySetFrameBuf) Z(sceDisplayWaitVblankStart) Z(sceGxmColorSurfaceInit) Z(sceGxmDisplayQueueAddEntry)
Z(sceGxmDisplayQueueFinish) Z(sceGxmInitialize) Z(sceGxmMapMemory) Z(sceGxmPadHeartbeat) Z(sceGxmSyncObjectCreate) Z(sceGxmSyncObjectDestroy)
Z(sceGxmTerminate) Z(sceGxmUnmapMemory) F(sceIoOpen) Z(sceIoClose) Z(sceIoMkdir) F(sceIoRead) Z(sceIoRemove) F(sceIoWrite)
Z(sceKernelFreeMemBlock) Z(sceKernelPowerTick) Z(sceNetAdhocInit) Z(sceNetAdhocTerm) Z(sceNetAdhocctlInit)
Z(sceNetAdhocctlTerm) Z(sceNetBind) Z(sceNetCheckDialogTerm) Z(sceNetCtlInetGetInfo) Z(sceNetCtlInetGetState) Z(sceNetCtlInit) Z(sceNetHtons)
Z(sceNetInetNtop) Z(sceNetInetPton) Z(sceNetInit) Z(sceNetRecvfrom) Z(sceNetSendto) Z(sceNetSetsockopt) Z(sceNetSocket) Z(sceNetSocketClose)
Z(sceSysmoduleLoadModule)
#include <stdlib.h>
int sceKernelAllocMemBlock() { return 5; }
int sceKernelGetMemBlockBase(int id, void **base) { (void)id; *base = calloc(1, 4 << 20); return 0; }
int sceKernelExitProcess(int r) { exit(r); }
#include <string.h>
void *sceClibMemset(void *d, int c, unsigned long n) { return memset(d, c, n); }
