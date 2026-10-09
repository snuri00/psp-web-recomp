# Translates the PRX modules a game loads at run time and writes the table the
# build links them through:
#   modules.py <psp_recomp> <disc_dir> <modules.txt> <generated_dir> [--siblings]
#
# modules.txt is written by the native runner: one line per module it loaded
# without code, "<path on the disc> <load address>". Each module is translated
# for that address into <generated_dir>/modules/<name>, in a namespace of its
# own, with unit numbers counted from the main executable's origin (so the
# runtime's fast unit table stays valid) and a range of import slots of its own.
#
# --siblings also translates every other PRX next to a listed one, at the same
# address, for games that load one level module at a time into the same place.
import os, re, subprocess, sys

recomp, disc, listing, generated = sys.argv[1:5]
siblings = '--siblings' in sys.argv[5:]
span = 0x4000


def key(path):
    return re.sub('/+', '/', path.replace('\\', '/')).strip('/').upper()


modules = {}
if os.path.exists(listing):
    for line in open(listing):
        parts = line.split()
        if len(parts) >= 2:
            modules[key(parts[0])] = (re.sub('/+', '/', parts[0]).strip('/'), int(parts[1], 16))
if siblings:
    for path, base in list(modules.values()):
        folder = os.path.dirname(path)
        for name in sorted(os.listdir(os.path.join(disc, folder))):
            if not name.upper().endswith('.PRX'):
                continue
            full = os.path.join(disc, folder, name)
            with open(full, 'rb') as f:
                if f.read(4) != b'\x7fELF':
                    continue  # encrypted firmware library, emulated
            modules.setdefault(key(folder + '/' + name), (folder + '/' + name, base))

# The main executable's unit origin, from any of its generated units.
origin = None
for name in sorted(os.listdir(generated)):
    if name.startswith('generated_unit_') and name.endswith('.cpp'):
        m = re.search(r'register_generated_unit\((\d+)u, (0x[0-9A-Fa-f]+)u, (\d+)u', open(os.path.join(generated, name)).read())
        if m:
            origin = int(m.group(2), 16) - int(m.group(1)) * int(m.group(3))
            break
if origin is None:
    sys.exit('no generated code for the main executable in ' + generated)

entries = []
for index, (k, (path, base)) in enumerate(sorted(modules.items())):
    namespace = 'm_' + re.sub('[^a-z0-9]', '_', os.path.splitext(os.path.basename(path))[0].lower())
    out = os.path.join(generated, 'modules', namespace)
    os.makedirs(out, exist_ok=True)
    print(f'== {path} at 0x{base:08X} -> modules/{namespace}', flush=True)
    subprocess.run([recomp, os.path.join(disc, path), '--auto', out, hex(base), hex(span),
                    '--namespace', namespace, '--unit-origin', hex(origin),
                    '--import-slot-base', str((index + 1) * 1024)], check=True, stdout=subprocess.DEVNULL)
    entries.append((path, base, namespace))

if not entries:
    sys.exit(f'nothing to translate: {listing} lists no modules')
table = ['// Written by scripts/modules.py: the run-time modules translated ahead of time.',
         '#include "../../host/generated_modules.hpp"', '',
         'namespace psprecomp {']
for _, _, namespace in entries:
    table.append(f'namespace {namespace} {{ void register_generated_functions(Runtime &runtime); }}')
table += ['}', '', 'namespace pspweb {', '', 'namespace {', 'const GeneratedModule kModules[] = {']
for path, base, namespace in entries:
    table.append(f'    {{"{path}", 0x{base:08X}u, &psprecomp::{namespace}::register_generated_functions}},')
table += ['};', '} // namespace', '',
          'std::span<const GeneratedModule> generated_modules() { return kModules; }', '',
          '} // namespace pspweb', '']
path = os.path.join(generated, 'modules_table.cpp')
text = '\n'.join(table)
if not os.path.exists(path) or open(path).read() != text:
    open(path, 'w').write(text)
print(f'{len(entries)} modules in {path}')
