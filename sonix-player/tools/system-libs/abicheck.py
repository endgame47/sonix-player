#!/usr/bin/env python3
"""abicheck.py ROOTFS EXTRA_DIR FILE...

For each ELF FILE: every NEEDED library must be found (in EXTRA_DIR first,
then ROOTFS/lib and ROOTFS/usr/lib), every undefined, non-weak symbol must be
defined by one of the libraries it loads (transitively), and every symbol
version it asks for must be one the defining library provides. Also prints
the ELF header flags and the MIPS FP ABI so they can be compared with stock.
"""
import os
import sys
from elftools.elf.elffile import ELFFile
from elftools.elf.dynamic import DynamicSection
from elftools.elf.gnuversions import GNUVerNeedSection, GNUVerDefSection, GNUVerSymSection

rootfs, extra = sys.argv[1], sys.argv[2]
SEARCH = [extra, os.path.join(rootfs, 'lib'), os.path.join(rootfs, 'usr/lib')]


def find(name):
    for d in SEARCH:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return os.path.realpath(p)
    return None


cache = {}


def info(path):
    if path in cache:
        return cache[path]
    with open(path, 'rb') as f:
        e = ELFFile(f)
        needed, soname = [], None
        for s in e.iter_sections():
            if isinstance(s, DynamicSection):
                for t in s.iter_tags():
                    if t.entry.d_tag == 'DT_NEEDED':
                        needed.append(t.needed)
                    elif t.entry.d_tag == 'DT_SONAME':
                        soname = t.soname
        dynsym = e.get_section_by_name('.dynsym')
        versym = e.get_section_by_name('.gnu.version')
        verdef = e.get_section_by_name('.gnu.version_d')
        verneed = e.get_section_by_name('.gnu.version_r')
        defnames = {}
        if verdef is not None:
            for vd, aux in verdef.iter_versions():
                names = [a.name for a in aux]
                defnames[vd['vd_ndx']] = names[0]
        neednames = {}
        if verneed is not None:
            for vn, aux in verneed.iter_versions():
                for a in aux:
                    neednames[a['vna_other']] = (vn.name, a.name)
        defined, undefined = {}, []
        if dynsym is not None:
            for i, sym in enumerate(dynsym.iter_symbols()):
                if i == 0:
                    continue
                ndx = versym.get_symbol(i)['ndx'] if versym is not None else 1
                hidden = False
                if isinstance(ndx, int) and ndx & 0x8000:
                    hidden = True
                    ndx &= 0x7fff
                if sym['st_shndx'] == 'SHN_UNDEF':
                    if sym.name:
                        undefined.append((sym.name, sym['st_info']['bind'], neednames.get(ndx)))
                else:
                    ver = defnames.get(ndx) if isinstance(ndx, int) else None
                    defined.setdefault(sym.name, set()).add(ver)
        flags = e.header['e_flags']
        fpabi = None
        att = e.get_section_by_name('.MIPS.abiflags')
        if att is not None:
            data = att.data()
            fpabi = data[7]
        r = dict(needed=needed, soname=soname, defined=defined, undefined=undefined,
                 flags=flags, fpabi=fpabi, verdefs=set(defnames.values()))
    cache[path] = r
    return r


def closure(path):
    seen, order = set(), []
    queue = [path]
    missing = []
    while queue:
        p = queue.pop(0)
        if p in seen:
            continue
        seen.add(p)
        order.append(p)
        for n in info(p)['needed']:
            q = find(n)
            if q is None:
                missing.append(n)
            else:
                queue.append(q)
    return order, missing


bad = 0
for path in sys.argv[3:]:
    i = info(path)
    libs, missing = closure(path)
    problems = ['NEEDED not found: ' + m for m in missing]
    for name, bind, need in i['undefined']:
        ok = False
        for lib in libs[1:]:
            d = info(lib)['defined'].get(name)
            if d is None:
                continue
            if need is None or need[1] in d or None in d:
                ok = True
                break
            problems.append('%s wants %s@%s, %s has %s' % (name, name, need[1], os.path.basename(lib), sorted(x for x in d if x)))
            ok = True
            break
        if not ok and bind != 'STB_WEAK':
            problems.append('undefined: %s%s' % (name, '@' + need[1] if need else ''))
    vers = sorted({n[1] for _, _, n in i['undefined'] if n})
    print('%s: flags=%#x fpabi=%s needed=%s' % (path, i['flags'], i['fpabi'], ' '.join(i['needed'])))
    print('    versions: %s' % ' '.join(vers))
    for p in problems:
        print('    PROBLEM ' + p)
        bad += 1
sys.exit(1 if bad else 0)
