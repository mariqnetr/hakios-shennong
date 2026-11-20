# userland/ in the shennong-ios volume is not in git, so these are scripted edits; each change is made once
import re
U = '/ios/hackios/userland/'

p = U + 'build.sh'
s = open(p).read()
old = 'cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/'
new = 'cp ../fw-audio/adsp*.mdt ../fw-audio/adsp*.b[0-9][0-9] sysroot/fw/     # (shennong: adsp_dtb too)'
if old in s:
    s = s.replace(old, new)
    open(p, 'w').write(s)
    print('build.sh: the system image takes every adsp*.mdt/.bNN of fw-audio')
elif new in s:
    print('build.sh: already so')
else:
    print('build.sh: THE LINE IS NOT THERE, not changed')
