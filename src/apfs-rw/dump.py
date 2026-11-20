import struct,sys
f=open(sys.argv[1],'rb'); BS=4096
def blk(n): f.seek(n*BS); return f.read(BS)
def hdr(b): oid,xid,typ,sub=struct.unpack_from('<QQII',b,8); return oid,xid,typ&0xffff,typ>>16,sub
nx=blk(0)
print('nx hdr',hdr(nx))
fld=struct.unpack_from('<IIQ16sQQ',nx,32)
print('bs,count,next_oid,next_xid',fld[1],fld[2],*struct.unpack_from('<QQ',nx,32+48))
# offsets from spec
o=32
names=['magic','block_size','block_count','features','ro_compat','incompat','uuid','next_oid','next_xid','xp_desc_blocks','xp_data_blocks','xp_desc_base','xp_data_base','xp_desc_next','xp_data_next','xp_desc_index','xp_desc_len','xp_data_index','xp_data_len','spaceman_oid','omap_oid','reaper_oid']
fmt='<4sIQQQQ16sQQIIqqIIIIII QQQ'.replace(' ','')
v=struct.unpack_from(fmt,nx,32)
for n,x in zip(names,v): print(' ',n,x if not isinstance(x,bytes) else x.hex())
d=dict(zip(names,v))
for i in range(d['xp_desc_blocks']):
    b=blk(d['xp_desc_base']+i)
    if b[:32]!=bytes(32) or any(b): print('desc',i,hdr(b))
for i in range(d['xp_data_blocks']):
    b=blk(d['xp_data_base']+i)
    if any(b): print('data',i,hdr(b))
