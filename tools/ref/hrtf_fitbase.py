# Dev-only: fits the shared HRTF base cascade (HRTF_BASE in src/audio/wa.c) to Chrome's frontal
# response. Input: lines "freq dB" measured in the browser.  python3 tools/ref/hrtf_fitbase.py front.txt
import math, random, sys
SR=48000
data=[tuple(map(float,l.split())) for l in open(sys.argv[1]) if l.strip()]
def coefs(t,f0,Q,G):
  A=10**(G/40); w0=2*math.pi*f0/SR; c=math.cos(w0); s=math.sin(w0)
  if t=='hp':
    al=s/(2*10**(Q/20)); b=((1+c)/2,-(1+c),(1+c)/2); a=(1+al,-2*c,1-al)
  else:
    al=s/(2*Q); b=(1+al*A,-2*c,1-al*A); a=(1+al/A,-2*c,1-al/A)
  return b,a
def mag_db(b,a,f):
  w=2*math.pi*f/SR; z1=complex(math.cos(-w),math.sin(-w)); z2=z1*z1
  return 20*math.log10(abs((b[0]+b[1]*z1+b[2]*z2)/(a[0]+a[1]*z1+a[2]*z2)))
NP=5
def model(p,f):
  g=p[0]
  g+=mag_db(*coefs('hp',p[1],p[2],0),f)
  for i in range(NP):
    fc,q,gg=p[3+3*i:6+3*i]
    g+=mag_db(*coefs('pk',fc,q,gg),f)
  return g
def err(p):
  if p[1]<10 or p[1]>400: return 1e9
  for i in range(NP):
    if not (30<p[3+3*i]<20000 and 0.2<p[4+3*i]<6 and abs(p[5+3*i])<15): return 1e9
  e=0
  for f,d in data:
    if f>18000: continue
    w=2.0 if f<600 else 1.0
    e+=w*(model(p,f)-d)**2
  return e
p0=[-5,20,-6,166,0.5,8,1160,5,-9,7100,1.5,-12,2300,1.5,2,12800,3,6]
def nm(p,it):
  n=len(p); simplex=[p[:]]
  for i in range(n):
    q=p[:]; q[i]=q[i]*1.1+ (0.5 if q[i]==0 else 0); simplex.append(q)
  vals=[err(x) for x in simplex]
  for _ in range(it):
    o=sorted(range(n+1),key=lambda i:vals[i]); simplex=[simplex[i] for i in o]; vals=[vals[i] for i in o]
    cen=[sum(x[j] for x in simplex[:-1])/n for j in range(n)]
    ref=[cen[j]+(cen[j]-simplex[-1][j]) for j in range(n)]; fr=err(ref)
    if fr<vals[0]:
      ex=[cen[j]+2*(cen[j]-simplex[-1][j]) for j in range(n)]; fe=err(ex)
      if fe<fr: simplex[-1],vals[-1]=ex,fe
      else: simplex[-1],vals[-1]=ref,fr
    elif fr<vals[-2]: simplex[-1],vals[-1]=ref,fr
    else:
      co=[cen[j]+0.5*(simplex[-1][j]-cen[j]) for j in range(n)]; fc=err(co)
      if fc<vals[-1]: simplex[-1],vals[-1]=co,fc
      else:
        for i in range(1,n+1):
          simplex[i]=[simplex[0][j]+0.5*(simplex[i][j]-simplex[0][j]) for j in range(n)]; vals[i]=err(simplex[i])
  i=min(range(n+1),key=lambda i:vals[i]); return simplex[i],vals[i]
p=p0; best=err(p)
for r in range(12):
  p,v=nm(p,3000); print('round',r,'rms err %.2f dB'%math.sqrt(v/len(data)))
print('params',[round(x,3) for x in p])
for f,d in data: print('%8.1f chrome %7.2f model %7.2f'%(f,d,model(p,f)))
