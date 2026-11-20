# userland/mkjobs.py in the shennong-ios volume is not in git, so this is not a 00NN patch
p = '/ios/hackios/userland/mkjobs.py'
s = open(p).read()
n = s.count("for name in ('RWA_PACKED16',):")
s = s.replace("""for name in ('RWA_PACKED16',):
            if os.environ.get(name):
                job.setdefault('EnvironmentVariables', {})[name] = os.environ[name]""",
"""for var in ('RWA_PACKED16',):
            if os.environ.get(var):
                job.setdefault('EnvironmentVariables', {})[var] = os.environ[var]""")
s = s.replace("""for name in ('RWA_PACKED16',):
        if os.environ.get(name):
            job.setdefault('EnvironmentVariables', {})[name] = os.environ[name]""",
"""for var in ('RWA_PACKED16',):
        if os.environ.get(var):
            job.setdefault('EnvironmentVariables', {})[var] = os.environ[var]""")
if "os.remove(f'{dest}/RWA_PACKED16')" not in s:
    s = s.replace("if rest:\n    count = 0\n",
                  "if rest:\n    count = 0\n    if os.path.isfile(f'{dest}/RWA_PACKED16'):\n        os.remove(f'{dest}/RWA_PACKED16')\n", 1)
open(p, 'w').write(s)
compile(s, p, 'exec')
print('mkjobs fixed:', n, 'loops renamed;', "for name in ('RWA_PACKED16'" not in s and 'ok' or 'STILL BROKEN')
