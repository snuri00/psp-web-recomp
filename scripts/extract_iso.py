# Extracts an ISO9660 image (optionally inside a .zip or .7z) to a directory:
#   extract_iso.py <image.iso|image.zip|image.7z> <out_dir>
import os, struct, subprocess, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
temporary = None
if src.lower().endswith('.zip'):
    zf = zipfile.ZipFile(src)
    member = next(n for n in zf.namelist() if n.lower().endswith('.iso'))
    f = zf.open(member)
elif src.lower().endswith('.7z'):
    # 7z streams cannot seek, so the image is unpacked next to the output first.
    listing = subprocess.run(['7z', 'l', '-slt', '-ba', src], capture_output=True, text=True, check=True).stdout
    member = next(line[7:] for line in listing.splitlines() if line.startswith('Path = ') and line.lower().endswith('.iso'))
    os.makedirs(out, exist_ok=True)
    temporary = os.path.join(os.path.dirname(os.path.abspath(out)), 'unpacked.iso')
    with open(temporary, 'wb') as o:
        subprocess.run(['7z', 'e', '-so', src, member], stdout=o, check=True)
    f = open(temporary, 'rb')
else:
    f = open(src, 'rb')
SEC = 2048
pos = 0
def read(lba, n):
    global pos
    target = lba * SEC
    if target < pos and not hasattr(f, 'raw'):  # zip streams are forward-only (rewinds are slow)
        pass
    f.seek(target); data = f.read(n); pos = target + len(data); return data
pvd = read(16, SEC)
assert pvd[1:6] == b'CD001', 'not an ISO9660 image'
root = pvd[156:190]
files, dirs = [], [('', struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0])]
while dirs:
    path, lba, size = dirs.pop(0)
    data, i = read(lba, size), 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // SEC + 1) * SEC; continue
        rec = data[i:i + n]; i += n
        name = rec[33:33 + rec[32]]
        if name in (b'\x00', b'\x01'): continue
        child = path + '/' + name.decode('ascii', 'replace').split(';')[0]
        elba, esize = struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]
        (dirs.append((child, elba, esize)) if rec[25] & 2 else files.append((child, elba, esize)))
total = 0
for path, lba, size in sorted(files, key=lambda e: e[1]):
    dst = os.path.join(out, path.lstrip('/'))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    f.seek(lba * SEC)
    with open(dst, 'wb') as o:
        left = size
        while left:
            chunk = f.read(min(left, 1 << 20)); o.write(chunk); left -= len(chunk)
    total += size
if temporary:
    f.close()
    os.remove(temporary)
print(f'extracted {len(files)} files, {total >> 20} MB to {out}')
