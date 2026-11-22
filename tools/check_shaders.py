# Compiles the VR renderer's HLSL (embedded in VRRender.cpp) with fxc, both with and without
# the single-pass stereo define, so a shader mistake fails the build instead of the game
# silently falling back.
import glob, os, re, subprocess, sys, tempfile

src = open(sys.argv[1], encoding='latin-1').read()
hlsl = re.search(r'R"HLSL\((.*?)\)HLSL"', src, re.S).group(1)
entries = re.findall(r'b(VS|PS)\("(\w+)"', src)
fxc = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe'))
if not fxc:
    print('fxc not found'); sys.exit(1)
fxc = fxc[-1]
path = os.path.join(tempfile.gettempdir(), 'sitebvr.hlsl')
open(path, 'w').write(hlsl)
bad = 0
for defs in ([], ['/D', 'STEREO_RT=1']):
    for kind, name in sorted(set(entries)):
        target = 'vs_5_0' if kind == 'VS' else 'ps_5_0'
        r = subprocess.run([fxc, '/nologo', '/T', target, '/E', name, '/O3', '/Fo', os.devnull] + defs + [path],
                           capture_output=True, text=True)
        if r.returncode != 0:
            bad += 1
            print('FAILED', name, defs, r.stdout, r.stderr)
print('shaders checked, failures:', bad)
sys.exit(1 if bad else 0)
