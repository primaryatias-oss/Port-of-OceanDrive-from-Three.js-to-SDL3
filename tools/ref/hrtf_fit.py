# Dev-only: refits the per-ear HRTF band tables in src/audio/wa.c (HRTF_NEAR / HRTF_FAR) to Chrome.
# Needs DIR/hrtf2.js: the band-level table measured in the browser (see c/tools/ref/probe.mjs usage in
# the port notes). Run from c/:  python3 tools/ref/hrtf_fit.py DIR ITERATIONS
import re, subprocess, sys
S=sys.argv[1]
src='src/audio/wa.c'
js=[l.split() for l in open(S+'/hrtf2.js') if l.strip()]
jref={r[0]:float(r[2]) for r in js if r[1]=='ref'}
J={(r[0],int(r[1])):(float(r[2])-jref[r[0]],float(r[3])-jref[r[0]]) for r in js if r[1]!='ref'}
B=['low','mid','high']
def read_tables(s):
  out={}
  for name in ['HRTF_NEAR','HRTF_FAR']:
    m=re.search(name+r'\[13\]\[3\] = \{(.*?)\n\};',s,re.S)
    nums=[float(x) for x in re.findall(r'-?\d+\.\d+|-?\d+',m.group(1))]
    out[name]=[nums[i*3:i*3+3] for i in range(13)]
  return out
def write_tables(s,t):
  for name in ['HRTF_NEAR','HRTF_FAR']:
    rows=t[name]
    body=''
    for i,r in enumerate(rows):
      if i%5==0: body+='\n  '
      else: body+=' '
      body+='{ %.2f, %.2f, %.2f },'%tuple(r)
    s=re.sub(name+r'\[13\]\[3\] = \{(.*?)\n\};',name+'[13][3] = {'+body+'\n};',s,flags=re.S)
  return s
for it in range(int(sys.argv[2])):
  r=subprocess.run(['make','-j16'],capture_output=True,text=True)
  if r.returncode: print(r.stdout[-2000:],r.stderr[-2000:]); sys.exit(1)
  o=subprocess.run(['build/release/oceandrive','--audio-test','hrtf'],capture_output=True,text=True).stdout
  c={};cref={}
  for l in o.splitlines():
    t=l.split()
    if len(t)<4: continue
    L=float(t[2][2:]);R=float(t[3][2:])
    if t[0].endswith('-nopan'): cref[t[0][:-6]]=L
    else: c[(t[0],int(t[1]))]=(L,R)
  s=open(src).read(); T=read_tables(s)
  worst=0
  for bi,b in enumerate(B):
    for k,az in enumerate(range(0,181,15)):
      cl,cr=c[(b,az)]; cl-=cref[b]; cr-=cref[b]
      jl,jr=J[(b,az)]
      ef, en = cl-jl, cr-jr
      worst=max(worst,abs(ef),abs(en))
      T['HRTF_FAR'][k][bi]-=ef
      T['HRTF_NEAR'][k][bi]-=en
  print('iteration',it,'worst error %.2f dB'%worst)
  open(src,'w').write(write_tables(s,T))
