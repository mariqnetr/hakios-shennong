import struct,sys
f=open(sys.argv[1],'rb'); BS=4096
def blk(n,c=1): f.seek(n*BS); return f.read(BS*c)
nx=blk(0)
dbase,=struct.unpack_from('<Q',nx,120); dnext,dlen=struct.unpack_from('<II',nx,144)
# find spaceman via data ring: index
didx,dl=struct.unpack_from('<II',nx,144)
for i in range(dl):
    b=blk(dbase+didx+i)
    t=struct.unpack_from('<I',b,24)[0]&0xffff
    if t==5: sm=b
u=lambda fmt,o: struct.unpack_from(fmt,sm,o)
print('bs,bpc,cpc,cpc2',u('<IIII',32))
for d,o in (('main',48),('tier2',96)):
    print(d,'blocks,chunks,cibs,cabs,free,addr_off',u('<QQIIQI',o))
print('flags,ipmult',u('<II',144),'ip_block_count',u('<Q',152),'ip_bm_size,ip_bm_count',u('<II',160),'ip_bm_base,ip_base',u('<QQ',168))
print('fs_reserve',u('<QQ',184))
for k in range(3): print('fq',k,'count,oid,oldest,limit',u('<QQQH',200+40*k))
print('ip_bm head/tail',u('<HH',320),'xid_off,bitmap_off,free_next_off',u('<III',324),'version,size',u('<II',336))
xo,bo,fo=u('<III',324)
ipbc,=u('<I',164)
print('ip bm xids',[u('<Q',xo+8*i)[0] for i in range(min(ipbc,8))],'bitmap offs',[u('<H',bo+2*i)[0] for i in range(min(ipbc,8))])
ao=u('<I',48+32)[0]
ncib=u('<I',48+16)[0]
cibs=[u('<Q',ao+8*i)[0] for i in range(ncib)]
print('cib addrs',cibs)
for c in cibs:
    b=blk(c); idx,cnt=struct.unpack_from('<II',b,32)
    print(' cib',c,'hdr',struct.unpack_from('<QQII',b,8),'index',idx,'count',cnt)
    for j in range(cnt):
        print('   ci',struct.unpack_from('<QQIIQ',b,40+32*j))
