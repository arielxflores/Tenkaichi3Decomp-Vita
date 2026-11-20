/*
 * Multiplayer over the internet (vita_net.c, "internet" matches): what lets the other player's Vita reach this one.
 *
 *   - The router (UPnP, the Internet Gateway Device protocol most home routers speak): asked through the local
 *     network to send the match's port (UDP 18630) on to this Vita while the match lasts, and what the router's own
 *     internet address is. The router is found by SSDP (a question sent to 239.255.255.250:1900), its description read
 *     over HTTP, and its WANIPConnection (or WANPPPConnection) service asked by SOAP: AddPortMapping at the start,
 *     DeletePortMapping at the end, GetExternalIPAddress. Done as miniupnpc does it, much shortened.
 *   - The internet address as the rest of the internet sees it (STUN, RFC 5389, a Binding Request to a public STUN
 *     server, sent from the match's own socket by vita_net.c): what the host tells the other player. When it differs
 *     from the router's own address, this network is behind a second router or the provider's (carrier-grade NAT),
 *     and nothing from outside can come in: the other player must host.
 *
 * All of it is plain BSD-style sockets (SceNet) with short time limits, on the start-up's text screen.
 */
#include <psp2/net/net.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Vita_Log(const char *fmt, ...);

#define SSDP_WAIT_US 2500000
#define HTTP_TIMEOUT_US 3000000
#define DESC_MAX 49152

static struct {
    int open;                 /* a port mapping was added (DeletePortMapping at the end) */
    int port;
    uint32_t ip;              /* the router's control address, network order */
    int ctlPort;
    char host[80];            /* "a.b.c.d:port", the Host header */
    char ctlPath[256];
    char service[96];         /* "urn:schemas-upnp-org:service:WANIPConnection:1" */
} sUp;

/* ------------------------------------------------------------------------------------------------- small helpers */
static uint64_t now_us(void) {
    return sceKernelGetProcessTimeWide();
}

/* a.b.c.d or a name: 1 and the address (network order) */
int Reach_Resolve(const char *name, uint32_t *ip) {
    SceNetInAddr a;
    int rid, r;
    if (sceNetInetPton(SCE_NET_AF_INET, name, &a) == 1) {
        *ip = a.s_addr;
        return 1;
    }
    rid = sceNetResolverCreate("bt3_dns", NULL, 0);
    if (rid < 0) {
        Vita_Log("reach: no name resolver (%08x)\n", (unsigned)rid);
        return 0;
    }
    memset(&a, 0, sizeof(a));
    r = sceNetResolverStartNtoa(rid, name, &a, 0, 0, 0);
    sceNetResolverDestroy(rid);
    if (r < 0 || a.s_addr == 0) {
        Vita_Log("reach: the name %s was not found (%08x)\n", name, (unsigned)r);
        return 0;
    }
    *ip = a.s_addr;
    return 1;
}

/* 10/8, 172.16/12, 192.168/16, 100.64/10 (carrier-grade NAT), 169.254/16, 127/8: not reachable from the internet */
int Reach_IsPrivate(uint32_t ip) {
    uint32_t h = sceNetNtohl(ip);
    return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 22) == (0x64400000u >> 22) || (h >> 16) == 0xA9FE ||
           (h >> 24) == 127 || h == 0;
}

static const char *find_ci(const char *s, const char *what) {
    size_t n = strlen(what);
    for (; *s != '\0'; s++) {
        if (strncasecmp(s, what, n) == 0) {
            return s;
        }
    }
    return NULL;
}

/* the text between <tag> and </tag> after `from` (namespaces before the tag's name allowed: <s:tag>), or 0 */
static int tag_text(const char *from, const char *tag, char *out, int size) {
    char open[48];
    const char *p = from, *b, *e;
    snprintf(open, sizeof(open), "%s>", tag);
    for (;;) {
        p = find_ci(p, open);
        if (p == NULL) {
            return 0;
        }
        if (p > from && (p[-1] == '<' || p[-1] == ':')) {
            break;
        }
        p++;
    }
    b = p + strlen(open);
    e = strchr(b, '<');
    if (e == NULL) {
        return 0;
    }
    while (b < e && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')) {
        b++;
    }
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        e--;
    }
    if (e - b >= size) {
        return 0;
    }
    memcpy(out, b, (size_t)(e - b));
    out[e - b] = '\0';
    return 1;
}

/* "http://host[:port][/path]": 1 with the parts (the path "/" when none) */
static int url_parts(const char *url, char *host, int hsize, int *port, char *path, int psize) {
    const char *p, *s, *c;
    if (strncasecmp(url, "http://", 7) != 0) {
        return 0;
    }
    p = url + 7;
    s = strchr(p, '/');
    if (s == NULL) {
        s = p + strlen(p);
    }
    c = memchr(p, ':', (size_t)(s - p));
    *port = c != NULL ? atoi(c + 1) : 80;
    if (c == NULL) {
        c = s;
    }
    if (c - p <= 0 || c - p >= hsize) {
        return 0;
    }
    memcpy(host, p, (size_t)(c - p));
    host[c - p] = '\0';
    snprintf(path, (size_t)psize, "%s", *s != '\0' ? s : "/");
    return *port > 0 && *port < 65536;
}

/* One HTTP exchange (Connection: close): the whole answer in out (its body de-chunked); the status code, or -1. */
static int http(uint32_t ip, int port, const char *req, int reqLen, char *out, int size, const char **body) {
    SceNetSockaddrIn a;
    int s, r, got = 0, timeo = HTTP_TIMEOUT_US, status = -1;
    char *b;

    *body = NULL;
    out[0] = '\0';
    s = sceNetSocket("bt3_upnp", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeo, sizeof(timeo));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeo, sizeof(timeo));
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons((unsigned short)port);
    a.sin_addr.s_addr = ip;
    if ((r = sceNetConnect(s, (SceNetSockaddr *)&a, sizeof(a))) < 0) {
        Vita_Log("reach: http: no connection to the router (%08x)\n", (unsigned)r);
        sceNetSocketClose(s);
        return -1;
    }
    while (reqLen > 0) {
        r = sceNetSend(s, req, (unsigned)reqLen, 0);
        if (r <= 0) {
            sceNetSocketClose(s);
            return -1;
        }
        req += r;
        reqLen -= r;
    }
    while (got < size - 1) {
        r = sceNetRecv(s, out + got, (unsigned)(size - 1 - got), 0);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    sceNetSocketClose(s);
    out[got] = '\0';
    if (got < 12 || strncasecmp(out, "HTTP/1.", 7) != 0) {
        return -1;
    }
    status = atoi(out + 9);
    b = strstr(out, "\r\n\r\n");
    if (b == NULL) {
        return status;
    }
    *b = '\0'; /* (the head ends here: searched for its headers below) */
    b += 4;
    if (find_ci(out, "transfer-encoding: chunked") != NULL) { /* (the chunks put together where they are) */
        char *w = b, *rd = b;
        for (;;) {
            char *end;
            long n = strtol(rd, &end, 16);
            char *line = strstr(rd, "\r\n");
            if (line == NULL || n <= 0 || line + 2 + n > out + got) {
                break;
            }
            memmove(w, line + 2, (size_t)n);
            w += n;
            rd = line + 2 + n;
            if (rd[0] == '\r' && rd[1] == '\n') {
                rd += 2;
            }
        }
        *w = '\0';
    }
    *body = b;
    return status;
}

/* ------------------------------------------------------------------------------------------------- the router: UPnP */
/* SSDP: the router's description's address ("http://..."), or 0 */
static int ssdp_find(char *loc, int size) {
    static const char *const kSt[3] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                       "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
    SceNetSockaddrIn a;
    char req[256], in[1536];
    int s, one = 1, ttl = 2, k, found = 0;
    uint64_t t0;

    s = sceNetSocket("bt3_ssdp", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
    if (s < 0) {
        return 0;
    }
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    sceNetSetsockopt(s, SCE_NET_IPPROTO_IP, SCE_NET_IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    sceNetBind(s, (SceNetSockaddr *)&a, sizeof(a));
    a.sin_port = sceNetHtons(1900);
    sceNetInetPton(SCE_NET_AF_INET, "239.255.255.250", &a.sin_addr);
    t0 = now_us();
    for (k = 0; !found && now_us() - t0 < SSDP_WAIT_US; k++) {
        int r;
        if (k % 40 == 0 && k / 40 < 6) { /* (each kind asked twice, half a second apart) */
            int n = snprintf(req, sizeof(req),
                             "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", kSt[(k / 40) % 3]);
            sceNetSendto(s, req, (unsigned)n, 0, (SceNetSockaddr *)&a, sizeof(a));
        }
        while ((r = sceNetRecv(s, in, sizeof(in) - 1, 0)) > 0) {
            const char *l, *e;
            in[r] = '\0';
            l = find_ci(in, "\nlocation:");
            if (l == NULL) {
                continue;
            }
            l += 10;
            while (*l == ' ' || *l == '\t') {
                l++;
            }
            e = strstr(l, "\r\n");
            if (e == NULL) {
                e = l + strlen(l);
            }
            if (e - l > 0 && e - l < size && strncasecmp(l, "http://", 7) == 0) {
                memcpy(loc, l, (size_t)(e - l));
                loc[e - l] = '\0';
                found = 1;
                break;
            }
        }
        sceKernelDelayThread(12500);
    }
    sceNetSocketClose(s);
    return found;
}

/* The router's description: its WAN connection service and where its control is. 1: found. */
static int upnp_service(const char *loc) {
    static char buf[DESC_MAX];
    char host[64], path[160], base[256], req[400], ctl[160];
    const char *body, *p;
    int port, n, st;
    uint32_t ip;

    if (!url_parts(loc, host, sizeof(host), &port, path, sizeof(path)) || !Reach_Resolve(host, &ip)) {
        return 0;
    }
    n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
    st = http(ip, port, req, n, buf, sizeof(buf), &body);
    if (st != 200 || body == NULL) {
        Vita_Log("reach: upnp: the router's description %s: HTTP %d\n", loc, st);
        return 0;
    }
    for (p = body;;) { /* (the first WAN IP or PPP connection service) */
        const char *t = find_ci(p, "urn:schemas-upnp-org:service:WANIPConnection:");
        const char *u = find_ci(p, "urn:schemas-upnp-org:service:WANPPPConnection:");
        const char *s = t != NULL && (u == NULL || t < u) ? t : u, *e;
        if (s == NULL) {
            Vita_Log("reach: upnp: the router has no WAN connection service\n");
            return 0;
        }
        e = s;
        while (*e != '\0' && *e != '<' && *e != ' ' && (e - s) < (int)sizeof(sUp.service) - 1) {
            e++;
        }
        memcpy(sUp.service, s, (size_t)(e - s));
        sUp.service[e - s] = '\0';
        if (tag_text(e, "controlURL", ctl, sizeof(ctl))) {
            break;
        }
        p = e;
    }
    /* the control's address: whole, or below the URLBase, or below the description's own host */
    if (strncasecmp(ctl, "http://", 7) == 0) {
        snprintf(base, sizeof(base), "%s", ctl);
    } else if (tag_text(body, "URLBase", base, sizeof(base)) && strncasecmp(base, "http://", 7) == 0) {
        size_t l = strlen(base);
        if (l > 0 && base[l - 1] == '/') {
            base[l - 1] = '\0';
        }
        snprintf(base + strlen(base), sizeof(base) - strlen(base), "%s%s", ctl[0] == '/' ? "" : "/", ctl);
    } else {
        snprintf(base, sizeof(base), "http://%s:%d%s%s", host, port, ctl[0] == '/' ? "" : "/", ctl);
    }
    if (!url_parts(base, host, sizeof(host), &sUp.ctlPort, sUp.ctlPath, sizeof(sUp.ctlPath)) || !Reach_Resolve(host, &sUp.ip)) {
        return 0;
    }
    snprintf(sUp.host, sizeof(sUp.host), "%s:%d", host, sUp.ctlPort);
    Vita_Log("reach: upnp: %s at http://%s%s\n", sUp.service, sUp.host, sUp.ctlPath);
    return 1;
}

/* One SOAP action of the router's service: the HTTP status; *body the answer's body (or NULL) */
static int soap(const char *action, const char *args, char *out, int size, const char **body) {
    static char req[2048];
    char env[1200];
    int e = snprintf(env, sizeof(env),
                     "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
                     action, sUp.service, args, action);
    int n = snprintf(req, sizeof(req),
                     "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                     sUp.ctlPath, sUp.host, sUp.service, action, e, env);
    return http(sUp.ip, sUp.ctlPort, req, n, out, size, body);
}

static int add_mapping(const char *myIp, int port, int lease, int *code) {
    static char out[4096];
    char args[600], err[16];
    const char *body;
    int st;
    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>BT3 Vita match</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
             port, port, myIp, lease);
    st = soap("AddPortMapping", args, out, sizeof(out), &body);
    *code = body != NULL && tag_text(body, "errorCode", err, sizeof(err)) ? atoi(err) : 0;
    Vita_Log("reach: upnp: AddPortMapping UDP %d -> %s, lease %d s: HTTP %d, error %d\n", port, myIp, lease, st, *code);
    return st == 200;
}

/* The router asked to send UDP `port` on to this Vita (`myIp`). 1: done. extIp: the router's own internet address
   ("" when it did not say); why: in words when it was not done. */
int Reach_UpnpOpen(const char *myIp, int port, char *extIp, int extSize, char *why, int whySize) {
    static char out[4096];
    char loc[200];
    const char *body;
    int code = 0, ok;

    memset(&sUp, 0, sizeof(sUp));
    extIp[0] = '\0';
    why[0] = '\0';
    if (!ssdp_find(loc, sizeof(loc))) {
        Vita_Log("reach: upnp: no router answered\n");
        snprintf(why, (size_t)whySize, "the router did not answer (UPnP is off in its settings, or it has none)");
        return 0;
    }
    Vita_Log("reach: upnp: the router's description at %s\n", loc);
    if (!upnp_service(loc)) {
        snprintf(why, (size_t)whySize, "the router answered, but not as expected (UPnP)");
        return 0;
    }
    if (soap("GetExternalIPAddress", "", out, sizeof(out), &body) == 200 && body != NULL) {
        tag_text(body, "NewExternalIPAddress", extIp, extSize);
        Vita_Log("reach: upnp: the router told its internet address\n");
    }
    ok = add_mapping(myIp, port, 7200, &code);
    if (!ok && code == 725) { /* (OnlyPermanentLeasesSupported) */
        ok = add_mapping(myIp, port, 0, &code);
    }
    if (!ok) {
        if (code == 718) {
            snprintf(why, (size_t)whySize, "the router gives the port %d to another device already", port);
        } else if (code == 606 || code == 401) {
            snprintf(why, (size_t)whySize, "the router does not let devices open ports (UPnP is set to read only)");
        } else {
            snprintf(why, (size_t)whySize, "the router said no (UPnP error %d)", code);
        }
        return 0;
    }
    sUp.open = 1;
    sUp.port = port;
    return 1;
}

/* the mapping taken away again (the match is over) */
void Reach_UpnpClose(void) {
    static char out[2048];
    char args[200];
    const char *body;
    int st;
    if (!sUp.open) {
        return;
    }
    sUp.open = 0;
    snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>", sUp.port);
    st = soap("DeletePortMapping", args, out, sizeof(out), &body);
    Vita_Log("reach: upnp: DeletePortMapping UDP %d: HTTP %d\n", sUp.port, st);
}

/* ------------------------------------------------------------------------------------------------- STUN */
static const uint8_t kStunId[12] = {'B', 'T', '3', 'V', 'i', 't', 'a', 'S', 't', 'u', 'n', '1'};

/* The servers' addresses (looked up once): up to `max`; their number. */
int Reach_StunServers(uint32_t *ip, uint16_t *port, int max) {
    static const struct { const char *name; uint16_t port; } kServers[3] = {
        {"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}, {"stun1.l.google.com", 19302}};
    static uint32_t sIp[3];
    static int sLooked;
    int k, n = 0;
    if (!sLooked) {
        sLooked = 1;
        for (k = 0; k < 3; k++) {
            if (!Reach_Resolve(kServers[k].name, &sIp[k])) {
                sIp[k] = 0;
            }
        }
    }
    for (k = 0; k < 3 && n < max; k++) {
        if (sIp[k] != 0) {
            ip[n] = sIp[k];
            port[n] = sceNetHtons(kServers[k].port);
            n++;
        }
    }
    return n;
}

/* a Binding Request: 20 bytes */
int Reach_StunRequest(uint8_t *out) {
    out[0] = 0x00;
    out[1] = 0x01; /* Binding Request */
    out[2] = out[3] = 0;
    out[4] = 0x21;
    out[5] = 0x12;
    out[6] = 0xA4;
    out[7] = 0x42; /* the magic cookie */
    memcpy(out + 8, kStunId, 12);
    return 20;
}

/* A Binding Response to our request: 1, with the address the server saw (network order). */
int Reach_StunTake(const uint8_t *p, int n, uint32_t *ip, uint16_t *port) {
    int k;
    if (n < 20 || p[0] != 0x01 || p[1] != 0x01 || p[4] != 0x21 || p[5] != 0x12 || p[6] != 0xA4 || p[7] != 0x42 || memcmp(p + 8, kStunId, 12) != 0) {
        return 0;
    }
    for (k = 20; k + 4 <= n;) {
        int type = p[k] << 8 | p[k + 1], len = p[k + 2] << 8 | p[k + 3];
        const uint8_t *v = p + k + 4;
        if (k + 4 + len > n) {
            break;
        }
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) {
            uint32_t a = (uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7];
            uint16_t o = (uint16_t)(v[2] << 8 | v[3]);
            if (type == 0x0020) {
                a ^= 0x2112A442u;
                o ^= 0x2112;
            }
            *ip = sceNetHtonl(a);
            *port = sceNetHtons(o);
            return 1;
        }
        k += 4 + ((len + 3) & ~3);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------- the host's code */
/* (1.2.6) The host's code: its internet address (and its port, when not 18630) in eight letters and digits, the last a
   check, in groups of four ("K7F2-9QXM"; twelve with a port). Crockford's base 32: no I, L, O or U, and I and L typed
   read as 1, O as 0, small letters as capitals. No server: the code is the address written shorter, the same each time
   for the same address. */
static const char kB32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int b32_val(char c) {
    const char *p;
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 32);
    }
    if (c == 'I' || c == 'L') {
        return 1;
    }
    if (c == 'O') {
        return 0;
    }
    p = c != '\0' ? strchr(kB32, c) : NULL;
    return p != NULL ? (int)(p - kB32) : -1;
}

/* (odd weights: one wrong letter always changes it) */
static int b32_check(const int *v, int n) {
    int k, sum = 0;
    for (k = 0; k < n; k++) {
        sum += (2 * k + 1) * v[k];
    }
    return sum % 32;
}

void Reach_CodeMake(uint32_t ip, int port, char *out, int size) {
    uint64_t x = sceNetNtohl(ip);
    int v[12], n = 0, k, o = 0, withPort = port != 18630;
    x |= (uint64_t)withPort << 32; /* (35 bits: the address and a mark for a port that follows) */
    for (k = 6; k >= 0; k--) {
        v[n++] = (int)((x >> (5 * k)) & 31);
    }
    if (withPort) {
        for (k = 3; k >= 0; k--) {
            v[n++] = (port >> (5 * k)) & 31;
        }
    }
    v[n] = b32_check(v, n);
    n++;
    for (k = 0; k < n && o < size - 2; k++) {
        if (k != 0 && k % 4 == 0) {
            out[o++] = '-';
        }
        out[o++] = kB32[v[k]];
    }
    out[o] = '\0';
}

/* text a code? 1: yes, *ip / *port from it; -1: it looks like one, with a mistake; 0: not a code (an address) */
int Reach_CodeRead(const char *text, uint32_t *ip, int *port) {
    int v[16], n = 0, k, withPort;
    uint64_t x = 0;
    for (; *text != '\0'; text++) {
        int d;
        if (*text == '-' || *text == ' ') {
            continue;
        }
        if (*text == '.' || *text == ':') {
            return 0;
        }
        d = b32_val(*text);
        if (d < 0 || n >= 16) {
            return 0;
        }
        v[n++] = d;
    }
    if (n != 8 && n != 12) {
        return 0;
    }
    if (b32_check(v, n - 1) != v[n - 1]) {
        return -1;
    }
    for (k = 0; k < 7; k++) {
        x = (x << 5) | (uint64_t)v[k];
    }
    withPort = (int)(x >> 32);
    if (withPort > 1 || (withPort == 1) != (n == 12)) {
        return -1;
    }
    *ip = sceNetHtonl((uint32_t)x);
    *port = 18630;
    if (withPort) {
        int p = 0;
        for (k = 7; k < 11; k++) {
            p = (p << 5) | v[k];
        }
        if (p <= 0 || p > 65535) {
            return -1;
        }
        *port = p;
    }
    return 1;
}
