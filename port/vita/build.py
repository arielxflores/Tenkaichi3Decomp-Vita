#!/usr/bin/env python3
"""PS Vita build of Tenkaichi3Decomp.

    python3 port/vita/build.py [compile] [link] [vpk]       (no step named: all three)

Environment:
    VITASDK            the VitaSDK installation (default /usr/local/vitasdk), with vitaGL built and installed from
                       port/vita/vitagl (build_vitagl.sh)
    VITA_SAMPLES       a checkout of https://github.com/vitasdk/samples (default: next to VITASDK)
    BT3_LIBCLANG       libclang's shared library, when it is not found in the usual places
    BT3_CLANG_PY       the folder that holds clang/cindex.py (libclang's Python bindings), when not installed
    BT3_VITA_DEV=1     the development build: its own title (BTTD00008); it reads every setting of bt3.txt (test
                       fights, statistics in the log), see docs/vita/development.md
    BT3_INCREMENTAL=1  reuse objects newer than their source (headers are not tracked)

The game's code is compiled as in the PC build's 32-bit variant: the same sources, the PS2 compiler's float
constants (eeconst.py) and call arguments evaluated right to left (ptr32.r2l). The game's built-in data tables come
from port/data as shape only; their values are read from the player's own disc when the program starts.
Objects go to port/build/obj_vita_<variant>, the package to port/build/vita_<variant>/."""
import concurrent.futures, os, pathlib, re, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "port/tools"))
sys.path.insert(0, str(ROOT / "port/vita"))
import portsrc, eeconst

SDK = pathlib.Path(os.environ.get("VITASDK", "/usr/local/vitasdk"))
P = str(SDK / "bin/arm-vita-eabi-")
DEV = os.environ.get("BT3_VITA_DEV") == "1"
TAG = "_dev" if DEV else "_rel"
OBJ = ROOT / ("port/build/obj_vita" + TAG)
OUT = ROOT / ("port/build/vita" + TAG)
GEN = ROOT / ("port/build/gen/vita" + TAG)
SCE_SYS = ROOT / "port/vita/sce_sys"
SAMPLES = pathlib.Path(os.environ.get("VITA_SAMPLES", SDK.parent / "samples"))  # debugScreen.c (CC0): the start-up text screen
TITLE_ID = "BTTD00008" if DEV else "BTTD00010"
TITLE = "BT3 (development)" if DEV else "Dragon Ball Z: BT3"
VPK = "Tenkaichi3Decomp-Vita-dev.vpk" if DEV else "Tenkaichi3Decomp-Vita.vpk"

# A few names exist both in the game's side and in the SDK's libraries with other meanings: the internals of the
# PS2's maths library (port/third_party/newlib_libm), the PS2 C library's character table, and the generic soft-float
# entry points of softfloat_ps2.c. Their definitions are renamed in those objects (objcopy --redefine-syms).
X_DEFS = ["addsf3", "subsf3", "mulsf3", "divsf3", "negsf2", "eqsf2", "nesf2", "ltsf2", "lesf2", "gtsf2", "gesf2", "unordsf2",
          "fixsfsi", "fixunssfsi", "floatsisf", "floatunsisf"]
RENAME = {f"__{n}": f"PortX_{n}" for n in X_DEFS}
RENAME |= {n: "PortNL_" + n.lstrip("_") for n in ["__ieee754_acosf", "__ieee754_asinf", "__ieee754_atan2f", "__ieee754_powf",
           "__ieee754_rem_pio2f", "__ieee754_sqrtf", "__kernel_cosf", "__kernel_rem_pio2f", "__kernel_sinf", "__kernel_tanf"]}
RENAME |= {"_ctype_": "PortPS2_ctype_"}
RENAME_SOFTFLOAT = {"RefVu0_AddBits": "PortX_VuAdd", "RefVu0_MulBits": "PortX_VuMul", "RefVu0_DivBits": "PortX_VuDiv"}

# What the PS2 compiler and 32-bit x86 gcc agree on and the ARM EABI does not: plain char is signed, enums are ints.
ABI = ["-fsigned-char", "-fno-short-enums", "-fsigned-bitfields"]
COMMON = ["-O2", "-g", "-fno-strict-aliasing", "-ffp-contract=off", "-w", "-DPORT_VITA=1", "-DPORT_VITA_FIXED=1",
          '-DPORT_VITA_SFX="-vgl.txt"' if DEV else '-DPORT_VITA_SFX=".txt"', "-DPORT_VITA_GFX=1", "-DPORT_VITA_VGL=1",
          "-DMPEG2_PLAYER=1", "-DPORT_VITA_SOUND=1", "-DPORT_VITA_NATIVE=1"] + ([] if DEV else ["-DPORT_VITA_RELEASE=1"])
# As in the PC build, no call of the C library is replaced by the compiler's own idea of it; the memory functions are
# allowed (small memcpy / memset calls become inline code).
NOBUILTIN = ["-fno-builtin-" + n for n in "sinf cosf tanf asinf acosf atanf atan2f sqrtf powf floorf ceilf fabsf fmodf expf logf sin cos tan sqrt "
             "pow floor ceil fabs abs labs rand srand printf sprintf snprintf fprintf vsprintf puts putchar fputs fwrite strcpy strncpy strcat "
             "strncat strlen strcmp strncmp strchr strrchr strstr malloc calloc realloc free exit abort atoi memcmp memmove".split()]
GAME = [P + "gcc", "-std=gnu89", "-c", "-fcommon"] + COMMON + ABI + NOBUILTIN + \
       ["-include", "port_libm.h", "-Iinclude", "-Iport/include", "-Iport/vita", "-include", "port_compat.h"]

# The game's compiler evaluates call arguments from right to left and the game's code depends on it in places; gcc
# for ARM goes left to right. The preprocessed text is rewritten as in the 64-bit PC build (ptr32.r2l, libclang).
R2L_ARGS = ["-x", "cpp-output", "--target=arm-none-eabi", "-mfloat-abi=soft", "-std=gnu89", "-fsigned-char", "-fno-short-enums", "-w",
            "-Wno-error=return-type", "-Wno-error=return-mismatch"]


def r2l(text):
    """The game's preprocessed text with call arguments evaluated right to left (ptr32.r2l) and the PS2's rule for
    shift counts written out (port/vita/srcfix.py). Returns (text, shifts rewritten)."""
    if os.environ.get("BT3_CLANG_PY"):
        sys.path.insert(0, os.environ["BT3_CLANG_PY"])
    import clang.cindex as ci
    if not ci.Config.loaded:
        lib = os.environ.get("BT3_LIBCLANG") or next((str(f) for d in ("/usr/lib/x86_64-linux-gnu", "/usr/lib/llvm-18/lib", "/usr/lib", "/usr/lib64")
                                                     for f in sorted(pathlib.Path(d).glob("libclang-*.so*")) + sorted(pathlib.Path(d).glob("libclang.so*"))), None)
        if lib:
            ci.Config.set_library_file(lib)
    import ptr32, srcfix
    data, n = srcfix.shifts(ptr32.r2l(text.encode("latin-1"), R2L_ARGS), R2L_ARGS)
    return data.decode("latin-1"), n


def compile_ee(cmd, src, o, game=False):
    """Preprocess, rewrite the float constants as the PS2 compiler read them (eeconst.py) and, for the game's
    sources, the calls whose arguments have side effects (r2l), compile."""
    r = subprocess.run([a for a in cmd if a != "-c"] + ["-E", str(src)], cwd=ROOT, capture_output=True, text=True, encoding="latin-1")
    if r.returncode:
        return r
    i = pathlib.Path(str(o)[:-2] + ".i")
    text = eeconst.transform(r.stdout)
    if game:
        try:
            text, nshift = r2l(text)
        except Exception as e:  # no libclang, or text it cannot parse
            return subprocess.CompletedProcess(cmd, 1, "", f"error: r2l: {e}\n")
    i.write_text(text, encoding="latin-1")
    return rename(subprocess.run(cmd + ["-fpreprocessed", str(i), "-o", str(o)], cwd=ROOT, capture_output=True, text=True), o)


def rename(r, o):
    """After a compile: the clashing definitions renamed (see RENAME)."""
    if r.returncode == 0:
        syms = RENAME | (RENAME_SOFTFLOAT if o.name == "pc_softfloat_ps2.o" else {})
        m = OBJ / "rename" / (o.name + ".txt")
        m.parent.mkdir(exist_ok=True)
        m.write_text("".join(f"{a} {b}\n" for a, b in syms.items()))
        r2 = subprocess.run([P + "objcopy", f"--redefine-syms={m}", str(o)], capture_output=True, text=True)
        if r2.returncode:
            return r2
    return r


def cc_game(f):
    o = OBJ / (str(f.relative_to(ROOT / "src")).replace("/", "_")[:-2] + ".o")
    src, done, missing = portsrc.prepare(f)
    if os.environ.get("BT3_INCREMENTAL") and o.exists() and o.stat().st_mtime > max(src.stat().st_mtime, f.stat().st_mtime):
        return o, f, ""
    r = compile_ee(GAME + [f"-I{f.parent}"], src, o, game=True)
    return (o if r.returncode == 0 else None), f, r.stderr


def first_error(e):
    return next((l for l in e.splitlines() if "rror" in l), e.strip()[:200])[:260]


def data_tables():
    """port/data/*.s assembled for ARM, each with a label at its first byte, and the list of where every run of
    values comes from on the player's disc as a C table (vita_dat.c)."""
    GEN.mkdir(parents=True, exist_ok=True)
    where, checksum = {}, None
    for l in (ROOT / "port/data/index.txt").read_text().splitlines():
        p = l.split()
        if len(p) == 3 and not l.startswith("#"):
            where[p[0]] = (int(p[1]), int(p[2], 16))
        elif len(p) == 2 and p[0] == "checksum":
            checksum = int(p[1], 16)
    objs, decl, rows, total = [], [], [], 0
    names = sorted(where) + sorted(f.name[:-2] for f in (ROOT / "port/data").glob("*.s") if f.name[:-2] not in where)  # the second lot: no values (.bss)
    for name in names:
        which, so = where.get(name, (None, 0))
        label = "PortTbl_" + re.sub(r"\W", "_", name)
        text = (ROOT / "port/data" / (name + ".s")).read_text()
        off, lines = 0, []
        for l in text.splitlines():
            lines.append(l)
            p = l.split()
            if p and p[0] == ".p2align" and label not in "\n".join(lines):
                lines += [f".globl {label}", f"{label}:"]
            if not p or p[0].startswith("#"):
                continue
            if p[0] == ".space":
                if which is None:
                    raise SystemExit(f"{name}: values expected from the disc, but the table is not in port/data/index.txt")
                rows.append(f"    {{{label} + {off}, {which}, 0x{so + off:X}, {int(p[1])}}},")
                total += int(p[1])
                off += int(p[1])
            elif p[0] == ".zero":
                off += int(p[1])
            elif p[0] == ".long":
                off += 4
        decl.append(f"extern unsigned char {label}[];")
        s = GEN / (name + ".s")
        s.write_text("\n".join(lines) + "\n")
        o = OBJ / ("data_" + re.sub(r"\W", "_", name) + ".o")
        r = rename(subprocess.run([P + "as", str(s), "-o", str(o)], capture_output=True, text=True), o)
        if r.returncode:
            print("FAILED", name, r.stderr.splitlines()[0][:200])
        else:
            objs.append(str(o))
    (GEN / "vita_dat.c").write_text(
        "/* generated by port/vita/build.py from port/data: where the game's data tables get their values */\n"
        '#include "vita_dat.h"\n' + "\n".join(decl) + "\nconst PortDatRun gPortDatRuns[] = {\n" + "\n".join(rows) +
        f"\n}};\nconst unsigned gPortDatCount = {len(rows)};\nconst unsigned long long gPortDatChecksum = 0x{checksum:X}ull;\n")
    print(f"data tables: {len(objs)} files, {total} bytes in {len(rows)} runs to read from the disc")
    return objs


def port_sources():
    """(file, kind): 'ee' game-side code (PS2 constants), 'gs' the renderer front end and sound (as on PC), 'hard' the
    Vita's own code, 'vu0' the vector-library references."""
    S, V = ROOT / "port/src", ROOT / "port/vita"
    out = [(ROOT / "src/port/vu0_a.c", "vu0"), (ROOT / "src/port/vu0_b.c", "vu0")]
    # of port/src: everything but the PC's memory, files, crash report and maths bridge (vita_*.c take their place)
    for n in ("gs_marker", "headless", "mathf_pc", "plat_gs", "plat_mc", "plat_movie", "plat_mpeg2",
              "plat_settings", "plat_stub", "plat_sys", "softfloat_ps2", "port_fast", "port_vufast"):
        out.append((S / (n + ".c"), "gs" if n in ("plat_movie", "plat_mpeg2") else "ee"))
    out += [(V / "vita_mem.c", "hard"), (V / "vita_file.c", "hard"), (V / "vita_main.c", "hard"), (V / "vita_float.c", "hard"),
            (V / "vita_eu.c", "hard"), (GEN / "vita_dat.c", "hard"), (S / "gs/gs_sony.c", "hard"),
            (SAMPLES / "common/debugScreen.c", "hard"),
            (V / "vita_gfx.c", "hard"), (S / "gs/gs_core.c", "gs"), (S / "gs/gs_vu1.c", "gs"),
            (V / "vita_perf.c", "hard"), (V / "vita_vgl.c", "hard"), (V / "vita_touch.c", "hard"), (V / "vita_optart.c", "hard"),
            (V / "vita_text.c", "hard"), (V / "vita_audio.c", "gs"), (S / "gs/snd_adx.c", "gs"), (S / "gs/snd_se.c", "gs"),
            (V / "vita_vufast_wrap.c", "hard")]
    return out


def cc_port(item):
    f, kind = item
    o = OBJ / ("pc_" + f.stem + ".o")
    base = [P + "gcc", "-std=gnu99", "-c"] + COMMON + ["-Iinclude", "-Iport/include", "-Iport/src", "-Iport/vita"]
    if kind == "gs":  # the PC's renderer front end and sound: ordinary float code; its environment switches answered at once
        o = OBJ / ("gs_" + f.stem + ".o")
        r = subprocess.run(base + ["-O3", "-freorder-blocks-and-partition", "-include", "vita_cold.h", "-Dgetenv=Vita_GsGetenv",
                                   f"-I{ROOT / 'port/src/gs'}", f"-I{ROOT / 'port/vita/sdlshim'}", str(f), "-o", str(o)], cwd=ROOT, capture_output=True, text=True)
    elif kind == "hard":
        r = subprocess.run(base + ["-O3", "-freorder-blocks-and-partition", "-include", "vita_cold.h", f"-I{SAMPLES / 'common'}", f"-I{GEN}",
                                   str(f), "-o", str(o)], cwd=ROOT, capture_output=True, text=True)
    elif kind == "vu0":  # the vector-library references under the game's names, with the three arithmetic primitives inline
        fast = ["-include", "vita_vu0_inline.h", "-DRefVu0_AddBits=VitaVu_AddBits", "-DRefVu0_MulBits=VitaVu_MulBits", "-DRefVu0_DivBits=VitaVu_DivBits"]
        if f.name == "vu0_b.c":
            fast += [f"-DRefVu0_{n}=VitaVuF_{n}" for n in ("Add", "Sub", "Mul", "Div")]
        mode = ["-O3", "--param", "inline-unit-growth=2000", "--param", "max-inline-insns-auto=400", "--param", "max-inline-insns-single=400",
                "--param", "large-function-growth=2000", "--param", "large-unit-insns=100000"]
        r = rename(subprocess.run(base + ABI + mode + fast + ["-include", "math.h", "-include", "stdlib.h", "-include", "port_libm.h",
                                                               "-include", "vu0_names.h", "-DREF_VU0_EXTERN_ARITH", str(f), "-o", str(o)],
                                  cwd=ROOT, capture_output=True, text=True), o)
    else:
        r = compile_ee(base + ABI + NOBUILTIN + ["-include", "math.h", "-include", "stdlib.h", "-include", "port_libm.h"], f, o)
    return (o if r.returncode == 0 else None), f, r.stderr


def cc_libm(f):
    o = OBJ / ("nl_" + f.stem + ".o")
    r = compile_ee([P + "gcc", "-std=gnu89", "-c"] + COMMON + ABI + NOBUILTIN + ["-Iport/include", f"-I{f.parent}", "-include", "newlib_shim.h"], f, o)
    return (o if r.returncode == 0 else None), f, r.stderr


def float_expected():
    """vita_float_expected.h: the reference sums of the float self-test (vita_float.c), computed on the host."""
    ren = [f"-D__{n}=PortX_{n}" for n in X_DEFS] + [f"-D{a}={b}" for a, b in RENAME_SOFTFLOAT.items()]
    cc = ["gcc", "-O2", "-w", "-Iinclude", "-Iport/include", "-Iport/vita"]
    for src, extra, o in (("port/src/softfloat_ps2.c", ren, "ft_soft.o"), ("src/port/vu0_a.c", ["-DREF_VU0_EXTERN_ARITH"], "ft_vu0.o"),
                          ("port/vita/floattest.c", [], "ft_main.o")):
        subprocess.run(cc + extra + ["-c", src, "-o", str(GEN / o)], cwd=ROOT, check=True)
    subprocess.run(["gcc", "-o", str(GEN / "floattest")] + [str(GEN / o) for o in ("ft_main.o", "ft_soft.o", "ft_vu0.o")] + ["-lm"], check=True)
    (GEN / "vita_float_expected.h").write_text(subprocess.run([str(GEN / "floattest")], check=True, capture_output=True, text=True).stdout)


def compile_all():
    OBJ.mkdir(parents=True, exist_ok=True)
    GEN.mkdir(parents=True, exist_ok=True)
    if not (SAMPLES / "common/debugScreen.c").exists():
        raise SystemExit(f"{SAMPLES}: the vitasdk samples are needed (git clone https://github.com/vitasdk/samples; VITA_SAMPLES=<folder>)")
    float_expected()
    data = data_tables()
    with concurrent.futures.ProcessPoolExecutor(os.cpu_count() or 4) as ex:
        res = list(ex.map(cc_game, portsrc.sources()))
        res += list(ex.map(cc_libm, sorted((ROOT / "port/third_party/newlib_libm").glob("*.c"))))
        res += list(ex.map(cc_port, port_sources()))
    bad = [(f, e) for o, f, e in res if o is None]
    for f, e in bad:
        print("FAILED", f.name, first_error(e))
    objs = [str(o) for o, _, _ in res if o] + data
    (OBJ / "objects.txt").write_text("\n".join(objs) + "\n")
    print(f"{len(objs)} objects, {len(bad)} failed")
    return not bad


LIBS = ["-lvitaGL", "-lstdc++", "-lSceCommonDialog_stub", "-lSceGxm_stub", "-lmathneon", "-lvitashark", "-lSceShaccCgExt", "-ltaihen_stub",
        "-lSceShaccCg_stub", "-lSceKernelDmacMgr_stub", "-lSceAudio_stub", "-lScePerf_stub_weak",
        "-lSceDisplay_stub", "-lSceCtrl_stub", "-lSceTouch_stub", "-lScePower_stub", "-lSceAppMgr_stub", "-lSceAppUtil_stub",
        "-lSceSysmodule_stub", "-lSceRtc_stub", "-lSceKernelThreadMgr_stub", "-lSceProcessmgr_stub", "-lSceLibKernel_stub",
        "-lSceIofilemgr_stub", "-lSceSysmem_stub", "-lpthread", "-lz", "-lm"]

# Game functions replaced by faster equivalents (port/src/port_fast.c: the stage's culling, the effects' task lists).
FAST_WRAPS = "StgFrustum_TestBox StgFrustum_TestPart BtlTaskList_Update BtlTaskList_Draw EftGfx_DrawPolyAvgZFront".split()
# Vector-library functions with an exact faster equivalent (port/src/port_vufast.c; the jumps: vita_vufast_wrap.c).
VUFAST_WRAPS = """ClipPoly_ClipPlane ClipPoly_ProjectCur Mtx_Mul Mtx_MulVec3 Mtx_MulVec4 Mtx_RotateX Mtx_RotateXYZ Mtx_RotateY Mtx_RotateZ
    Mtx_RotateZXY Vec3_Add Vec3_Cross Vec3_Dist Vec3_DistSq Vec3_Dot Vec3_Length Vec3_LengthSq Vec3_Normalize Vec3_Scale Vec3_Sub Vec4_Add
    Vec4_Mul Vec4_Scale Vec4_Sub Vu0Cur_MulMtx Vu0Cur_MulMtxRev Vu0Cur_MulVec3 Vu0Cur_MulVec4 Vu0Cur_ProjectInt Vu0Cur_RotateX
    Vu0Cur_RotateXYZ Vu0Cur_RotateY Vu0Cur_RotateZ Vu0Cur_RotateZXY""".split()
# The port's own entry points around the game's (headless.c, vita_main.c) and the renderer's hooks.
PORT_WRAPS = "Progress_Main main exit Port_GsVif1Chain sceMpegGetPicture GsVu1_Call".split()


def link_cmd(objs, extra):
    # -q keeps the relocations vita-elf-create needs (a Vita program is loaded at an address of the system's choosing)
    wraps = [f"-Wl,--wrap={n}" for n in PORT_WRAPS + FAST_WRAPS + VUFAST_WRAPS]
    return [P + "gcc", "-Wl,-q", "-Wl,--no-warn-mismatch", "-w", "-o", str(OUT / "bt3.elf")] + extra + objs + LIBS + wraps + \
           [f"-Wl,-Map={OUT / 'bt3.map'}"]


def link():
    OUT.mkdir(parents=True, exist_ok=True)
    objs = (OBJ / "objects.txt").read_text().split()
    r = subprocess.run(link_cmd(objs, []), capture_output=True, text=True)
    # functions the game's code refers to that do not exist on the Vita (the PS2's hardware libraries): each gets a
    # stub that stops the program with its name, should it ever be called
    need = sorted(set(re.findall(r"undefined reference to `(\w+)'", r.stderr)))
    lines = ["/* Generated by port/vita/build.py: one stub per symbol nothing provides. */", "extern void Port_Missing(const char *name);"]
    lines += [f'void {s}(void) {{ Port_Missing("{s}"); }}' for s in need]
    (GEN / "stubs.c").write_text("\n".join(lines) + "\n")
    so = OBJ / "pc_stubs.o"
    subprocess.run([P + "gcc", "-c", "-O1", str(GEN / "stubs.c"), "-o", str(so)], check=True)
    # vita-elf-create needs room behind the first segment for its own tables; when the segment ends just before a page
    # boundary there is none ("segment 1 overlaps"), and a few constant bytes are added until it fits
    pad = []
    for size in range(0, 4096, 512):
        if size:
            (GEN / "pad.c").write_text(f"const char gVitaPad[{size}] = {{1}};\n")
            subprocess.run([P + "gcc", "-c", str(GEN / "pad.c"), "-o", str(OBJ / "pad.o")], check=True)
            pad = [str(OBJ / "pad.o")]
        r = subprocess.run(link_cmd(objs, [str(so)] + pad), capture_output=True, text=True)
        if r.returncode != 0:
            break
        t = subprocess.run([str(SDK / "bin" / "vita-elf-create"), str(OUT / "bt3.elf"), str(OUT / "probe.velf")], capture_output=True, text=True)
        if t.returncode == 0:
            break
    errs = [l for l in r.stderr.splitlines() if "multiple definition" in l or "undefined reference" in l or "rror" in l]
    print(f"{len(need)} stubs; link {'OK' if r.returncode == 0 else 'FAILED'}")
    for l in errs[:15]:
        print("  " + l[-200:])
    return r.returncode == 0


def vpk():
    run = lambda *a: subprocess.run([str(SDK / "bin" / a[0])] + [str(x) for x in a[1:]], check=True, capture_output=True, text=True)
    shutil.copy(OUT / "bt3.elf", OUT / "bt3.unstripped.elf")
    subprocess.run([P + "strip", "-g", str(OUT / "bt3.elf")], check=True)
    run("vita-elf-create", OUT / "bt3.elf", OUT / "bt3.velf")
    run("vita-make-fself", "-c", OUT / "bt3.velf", OUT / "eboot.bin")
    run("vita-mksfoex", "-s", f"TITLE_ID={TITLE_ID}", "-d", "ATTRIBUTE2=12", TITLE, OUT / "param.sfo")
    extra = []
    for f in sorted(x for x in SCE_SYS.rglob("*") if x.is_file()):
        extra += ["-a", f"{f}=sce_sys/{f.relative_to(SCE_SYS).as_posix()}"]
    run("vita-pack-vpk", "-s", OUT / "param.sfo", "-b", OUT / "eboot.bin", *extra, OUT / VPK)
    print(f"{OUT / VPK}: {(OUT / VPK).stat().st_size} bytes")


def main():
    steps = [a for a in sys.argv[1:] if not a.startswith("-")] or ["compile", "link", "vpk"]
    if "compile" in steps and not compile_all():
        sys.exit(1)
    if "link" in steps and not link():
        sys.exit(1)
    if "vpk" in steps:
        vpk()


if __name__ == "__main__":
    main()
