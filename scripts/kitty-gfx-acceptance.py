#!/usr/bin/env python3

"""Acceptance test: compare paw's kitty graphics replies with kitty's.

Each case writes graphics commands to the terminal, then asks for the
cursor position (CSI 6n) and the primary device attributes (CSI c). The
result of a case is the list of graphics replies, the cursor position,
and, for file cases, whether the files still exist.

  record   run the cases in kitty and write the golden file
  check    run the cases in paw and compare with the golden file
  run OUT  (internal) run the cases in the current terminal

Error replies are compared by code only (the text before ':'), as kitty's
own tests do, except the generic file error, which must match in full.
KNOWN_DIFFS lists the cases where paw differs from kitty on purpose.
"""

import argparse
import base64
import fnmatch
import json
import os
import re
import select
import subprocess
import sys
import tempfile
import termios
import tty
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCRIPT = Path(__file__).resolve()
GOLDEN = SCRIPT.parent.parent / 'tests' / 'kitty-gfx-golden.json'
FILE_ERROR = 'EBADF:Failed to read image file'
MiB = 1 << 20

# Cases where the add-kitty-graphics spec differs from kitty on purpose.
# These values replace the golden ones. 'E*' is any error.
KNOWN_DIFFS: dict[str, dict[str, Any]] = {
    # Out of scope: kitty accepts animation and relative placements.
    'anim_frame': {'replies': ['i=1;E*']},
    'anim_control': {'replies': ['i=1;E*']},
    'anim_compose': {'replies': ['i=1;E*']},
    'relative_placement': {'replies': ['i=1,p=1;OK', 'i=1,p=2;E*']},
    # Spec: EINVAL reply. kitty drops the command without a reply.
    'bad_number': {'replies': ['i=1;EINVAL']},
    # Spec: any o other than z is an error. kitty drops the command.
    'bad_compression': {'replies': ['i=7;E*']},
    # Spec: a file shorter than O+S fails. kitty reads what is there.
    'file_short': {'replies': ['i=1;' + FILE_ERROR]},
    # Spec: an image over the quota fails and deletes nothing. kitty
    # stores it and then evicts every image, the new one too.
    'quota_too_large': {'replies': ['i=2;E*', 'i=1;OK', 'i=2;ENOENT']},
    # Spec: the cursor move scrolls at the scroll region bottom (row 10).
    # kitty moves the cursor past the region.
    'put_scrolls_region': {'cursor': [10, 3]},
    # kitty writes ',I=7' (empty i). Any form that ends in I=7 is good.
    'delete_number': {'replies': ['*I=7;ENOENT']},
}


def g(ctrl: str, payload: bytes = b'') -> bytes:
    data = b';' + base64.standard_b64encode(payload) if payload else b''
    return b'\x1b_G' + ctrl.encode() + data + b'\x1b\\'


def chunked(ctrl: str, payload: bytes) -> list[bytes]:
    b64 = base64.standard_b64encode(payload)
    parts = [b64[i:i + 4096] for i in range(0, len(b64), 4096)]
    out = []
    for n, part in enumerate(parts):
        last = n == len(parts) - 1
        keys = (ctrl + ',' if n == 0 else '') + f'm={0 if last else 1}'
        out.append(b'\x1b_G' + keys.encode() + b';' + part + b'\x1b\\')
    return out


def png(w: int, h: int) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(kind + data).to_bytes(4, 'big')
        return len(data).to_bytes(4, 'big') + kind + data + crc

    raw = b''.join(b'\0' + b'\xff\x00\x00\xff' * w for _ in range(h))
    ihdr = w.to_bytes(4, 'big') + h.to_bytes(4, 'big') + bytes([8, 6, 0, 0, 0])
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def zeros_z(size: int) -> bytes:
    """zlib data that inflates to `size` zero bytes (a multiple of 1 MiB)."""
    z = zlib.compressobj(9)
    block = bytes(MiB)
    return b''.join([z.compress(block) for _ in range(size // MiB)] + [z.flush()])


def cup(row: int, col: int) -> bytes:
    return f'\x1b[{row};{col}H'.encode()


RGB1 = bytes(3)
T1 = 'a=t,f=24,s=1,v=1,q=2'  # quiet 1x1 transmit, add i=


@dataclass
class Case:
    name: str
    steps: list[bytes]
    files: dict[str, Path] = field(default_factory=dict)


def transmission_cases() -> list[Case]:
    z40 = zeros_z(40 * MiB)
    return [
        Case('query_rgb', [g('a=q,i=31,s=1,v=1,f=24', RGB1)]),
        Case('query_keeps_image', [
            g(T1 + ',i=31', RGB1),
            g('a=q,i=31,s=2,v=2,f=24', RGB1),
            g('a=p,i=31,C=1')]),
        Case('other_strings_ignored',
             [b'\x1b_hello\x1b\\A\x1bXsos\x1b\\B\x1b^pm\x1b\\C']),
        Case('bad_number', [g('a=q,i=1,s=x,v=1,f=24', RGB1)]),
        Case('unknown_key', [g('a=q,i=1,s=1,v=1,f=24,N=1', RGB1)]),
        Case('rgb', [g('a=t,i=7,f=24,s=10,v=20', bytes(600))]),
        Case('rgba_wrong_length', [g('a=t,i=7,f=32,s=10,v=10', bytes(300))]),
        Case('png', [g('a=t,i=7,f=100', png(2, 2))]),
        Case('png_invalid', [g('a=t,i=7,f=100', b'not a png at all')]),
        Case('zlib', [g('a=t,i=7,f=32,s=4,v=4,o=z', zlib.compress(bytes(64)))]),
        Case('zlib_corrupt', [g('a=t,i=7,f=32,s=4,v=4,o=z', b'\x78\x9cgarbage')]),
        Case('bad_compression', [g('a=t,i=7,f=32,s=1,v=1,o=x', bytes(4))]),
        Case('chunked_three', [
            g('a=t,i=7,f=32,s=4,v=4,m=1', bytes(15)),
            g('m=1', bytes(30)),
            g('m=0', bytes(19))]),
        Case('chunked_cancel', [
            g('a=t,i=7,f=32,s=4,v=4,m=1', bytes(15)),
            g('a=d'),
            g('m=0', bytes(49)),
            g('a=p,i=7,C=1')]),
        Case('chunked_large', chunked('a=t,i=8,f=32,s=64,v=64', bytes(16384))),
        Case('image_number', [
            g('a=t,I=13,f=24,s=1,v=1', RGB1),
            g('a=p,I=13,C=1'),
            g('a=t,i=1,I=2,f=24,s=1,v=1', RGB1)]),
        Case('quiet_ok', [g('a=t,i=7,q=1,f=24,s=1,v=1', RGB1)]),
        Case('quiet_error_q1', [g('a=p,i=99,q=1')]),
        Case('quiet_error_q2', [g('a=p,i=99,q=2')]),
        Case('place_missing', [g('a=p,i=99')]),
        Case('no_id_no_reply', [
            g('a=t,f=24,s=1,v=1', RGB1),
            g('a=t,f=24,s=2,v=2', RGB1)]),
        # 9 x 40 MiB > 320 MiB: the oldest image goes.
        Case('quota_eviction', [
            step for i in range(1, 10)
            for step in chunked(f'a=t,i={i},f=32,s=4096,v=2560,o=z', z40)
        ] + [g('a=p,i=1,C=1'), g('a=p,i=9,C=1')]),
        # 9216 x 9216 x 4 = 324 MiB, more than the quota on its own.
        Case('quota_too_large', [g(T1 + ',i=1', RGB1)]
             + chunked('a=t,i=2,f=32,s=9216,v=9216,o=z', zeros_z(324 * MiB))
             + [g('a=p,i=1,C=1'), g('a=p,i=2,C=1')]),
        Case('anim_frame', [g(T1 + ',i=1', RGB1), g('a=f,i=1,f=24,s=1,v=1', RGB1)]),
        Case('anim_control', [g(T1 + ',i=1', RGB1), g('a=a,i=1,s=1')]),
        Case('anim_compose', [g(T1 + ',i=1', RGB1), g('a=c,i=1,r=1,c=1')]),
        Case('relative_placement', [
            g(T1 + ',i=1', RGB1),
            g('a=p,i=1,p=1,C=1'),
            g('a=p,i=1,p=2,P=1,Q=1,C=1')]),
    ]


def file_cases(tmp: Path) -> list[Case]:
    def path(name: str, data: bytes) -> Path:
        p = tmp / name
        p.write_bytes(data)
        return p

    def f(ctrl: str, p: Path | str) -> bytes:
        return g(ctrl, str(p).encode())

    img = path('img.png', png(2, 2))
    forty = path('forty.bin', bytes(range(40)))
    fifty = path('fifty.bin', bytes(50))
    (tmp / 'environ').symlink_to('/proc/self/environ')
    (tmp / 'loop-a').symlink_to(tmp / 'loop-b')
    (tmp / 'loop-b').symlink_to(tmp / 'loop-a')
    os.mkfifo(tmp / 'fifo')
    sfx = tmp.name
    tmp_ok = path(f'/tmp/tty-graphics-protocol-{sfx}.png', png(2, 2))
    tmp_plain = path(f'/tmp/photo-{sfx}.png', png(2, 2))
    home = path(f'{Path.home()}/tty-graphics-protocol-{sfx}.png', png(2, 2))
    shm_ok = path(f'/dev/shm/paw-gfx-{sfx}', bytes(8 * 8 * 4))
    shm_short = path(f'/dev/shm/paw-gfx-short-{sfx}', bytes(10))
    rgb22 = 'f=24,s=2,v=2,i=1'

    return [
        Case('file_png', [f('a=T,t=f,f=100,i=1,C=1', img)], {'img': img}),
        Case('file_offset', [f(f'a=t,t=f,{rgb22},O=10,S=12', forty)]),
        Case('file_short', [f(f'a=t,t=f,{rgb22},O=0,S=100', fifty)]),
        Case('file_dev_zero', [f(f'a=t,t=f,{rgb22}', '/dev/zero')]),
        Case('file_proc_link', [f(f'a=t,t=f,{rgb22}', tmp / 'environ')]),
        Case('file_fifo', [f(f'a=t,t=f,{rgb22}', tmp / 'fifo')]),
        Case('file_loop', [f(f'a=t,t=f,{rgb22}', tmp / 'loop-a')]),
        Case('file_missing', [f(f'a=t,t=f,{rgb22}', tmp / 'missing')]),
        Case('temp_file', [f('a=t,t=t,f=100,i=1', tmp_ok)], {'file': tmp_ok}),
        Case('temp_no_marker', [f('a=t,t=t,f=100,i=1', tmp_plain)], {'file': tmp_plain}),
        Case('temp_in_home', [f('a=t,t=t,f=100,i=1', home)], {'file': home}),
        Case('shm', [f('a=t,t=s,f=32,s=8,v=8,i=1', '/' + shm_ok.name)], {'shm': shm_ok}),
        Case('shm_short', [f('a=t,t=s,f=32,s=8,v=8,i=1', '/' + shm_short.name)],
             {'shm': shm_short}),
        Case('shm_bad_name', [f('a=t,t=s,f=32,s=8,v=8,i=1', '/a/b')]),
    ]


def placement_cases() -> list[Case]:
    cases = [
        Case('put_cursor_move', [cup(3, 5), g('a=T,i=1,f=24,s=1,v=1,c=4,r=3', RGB1)]),
        Case('put_no_cursor_move', [cup(3, 5), g('a=T,i=1,f=24,s=1,v=1,c=4,r=3,C=1', RGB1)]),
        Case('put_pixel_offset', [cup(3, 5), g('a=T,i=1,f=24,s=1,v=1,X=1,Y=1', RGB1)]),
        # Scroll region rows 1-10: a 5-row image put on row 8 scrolls it.
        Case('put_scrolls_region', [b'\x1b[1;10r', cup(8, 1),
                                    g('a=T,i=1,f=24,s=1,v=1,c=2,r=5', RGB1)]),
        Case('put_replace', [g(T1 + ',i=1', RGB1), g('a=p,i=1,p=2,C=1'),
                             cup(5, 5), g('a=p,i=1,p=2,C=1')]),
        Case('put_source_rect', [g('a=t,i=1,f=32,s=4,v=4,q=2', bytes(64)),
                                 g('a=p,i=1,x=2,y=0,w=2,h=4,c=2,r=2')]),
        Case('put_under_text', [g(T1 + ',i=1', RGB1), g('a=p,i=1,z=-1,C=1')]),
        Case('put_under_bg', [g(T1 + ',i=1', RGB1), g('a=p,i=1,z=-1073741825,C=1')]),
        Case('clear_screen', [g(T1 + ',i=1', RGB1), g('a=p,i=1,c=2,r=2'),
                          b'\x1b[2J', g('a=p,i=1,C=1')]),
        Case('alt_screen', [
            g(T1 + ',i=1', RGB1),
            b'\x1b[?1049h', g('a=p,i=1,C=1'), g(T1 + ',i=2', RGB1),
            b'\x1b[?1049l', g('a=p,i=2,C=1'), g('a=p,i=1,C=1'),
            b'\x1b[?1049h', g('a=p,i=2,C=1'), b'\x1b[?1049l']),
        Case('reset', [g(T1 + ',i=1', RGB1), b'\x1bc', g('a=p,i=1,C=1')]),
        Case('delete_number', [g('a=t,I=7,f=24,s=1,v=1,q=2', RGB1),
                               g('a=p,I=7,C=1,q=2'), g('a=d,d=N,I=7'),
                               g('a=p,I=7,C=1')]),
    ]
    # Image 4 is put at row 3, column 5 (2x2 cells). Each selector hits it.
    # Lowercase keeps the image data, uppercase frees it.
    hits = {'a': '', 'i': ',i=4', 'c': '', 'p': ',x=5,y=3', 'q': ',x=6,y=4,z=0',
            'r': ',x=1,y=10', 'x': ',x=6', 'y': ',y=4', 'z': ',z=0'}
    for sel, keys in hits.items():
        for d in (sel, sel.upper()):
            cases.append(Case(f'delete_{d}', [
                g(T1 + ',i=4', RGB1), cup(3, 5), g('a=p,i=4,c=2,r=2,q=2'), cup(3, 5),
                g(f'a=d,d={d}{keys}'), g('a=p,i=4,C=1')]))
    cases += [
        Case('delete_miss', [g(T1 + ',i=4', RGB1), cup(3, 5), g('a=p,i=4,c=2,r=2,q=2'),
                             g('a=d,d=X,x=40'), g('a=p,i=4,C=1')]),
        Case('delete_frames', [g(T1 + ',i=4', RGB1), g('a=d,d=f'), g('a=p,i=4,C=1')]),
    ]
    return cases


def placeholder_cases() -> list[Case]:
    cell = '\U0010eeee'
    return [
        Case('virtual_put', [cup(3, 5), g('a=T,U=1,i=42,c=2,r=2,f=24,s=1,v=1', RGB1)]),
        Case('virtual_quiet', [g(T1 + ',i=42', RGB1), g('a=p,U=1,i=42,c=2,r=2,q=2')]),
        Case('virtual_survives_delete_all', [
            g(T1 + ',i=42', RGB1), g('a=p,U=1,i=42,c=2,r=2,q=2'),
            g('a=d,d=A'), g('a=p,i=42,C=1')]),
        Case('placeholder_cells', [
            g(T1 + ',i=42', RGB1), g('a=p,U=1,i=42,c=2,r=2,q=2'), cup(3, 5),
            f'\x1b[38;5;42m{cell}̅̅{cell}̅̍\x1b[m'.encode()]),
    ]


def read_until_da1(fd: int, timeout: float) -> bytes:
    buf = b''
    while not re.search(rb'\x1b\[\?[0-9;]*c', buf):
        ready, _, _ = select.select([fd], [], [], timeout)
        if not ready:
            raise TimeoutError(f'no DA1 reply after {timeout}s: {buf!r}')
        buf += os.read(fd, 65536)
    return buf


def write_all(fd: int, data: bytes) -> None:
    while data:
        data = data[os.write(fd, data):]


def run_case(fd: int, case: Case) -> dict[str, Any]:
    write_all(fd, b'\x1bc')  # RIS: start every case from a clean terminal
    for step in case.steps:
        write_all(fd, step)
    write_all(fd, b'\x1b[6n\x1b[c')
    buf = read_until_da1(fd, 120.0)
    cpr = re.search(rb'\x1b\[(\d+);(\d+)R', buf)
    return {
        'replies': [r.decode() for r in re.findall(rb'\x1b_G([^\x1b]*)\x1b\\', buf)],
        'cursor': [int(cpr[1]), int(cpr[2])] if cpr else None,
        'files': {k: p.exists() for k, p in case.files.items()},
    }


def run(out: Path) -> None:
    with tempfile.TemporaryDirectory(prefix='paw-gfx-', dir='/tmp') as tmp:
        cases = (transmission_cases() + file_cases(Path(tmp))
                 + placement_cases() + placeholder_cases())
        fd = os.open('/dev/tty', os.O_RDWR)
        attrs = termios.tcgetattr(fd)
        results = {}
        try:
            tty.setraw(fd)
            for case in cases:
                results[case.name] = run_case(fd, case)
                for p in case.files.values():
                    p.unlink(missing_ok=True)
            os.write(fd, b'\x1bc')
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, attrs)
            os.close(fd)
    out.write_text(json.dumps(results, indent=1) + '\n')


def launch(argv: list[str]) -> dict[str, Any]:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'results.json'
        subprocess.run(argv + [sys.executable, str(SCRIPT), 'run', str(out)], check=True)
        if not out.exists():
            sys.exit(f'{argv[0]}: the run wrote no results')
        results: dict[str, Any] = json.loads(out.read_text())
        return results


def norm(reply: str) -> str:
    """Keep only the error code of a reply, as kitty's tests compare it."""
    keys, _, msg = reply.partition(';')
    return f"{keys};{msg if msg in ('OK', FILE_ERROR) else msg.partition(':')[0]}"


def pattern(reply: str) -> str:
    """A golden reply as an fnmatch pattern. The terminal picks the id for I=."""
    reply = norm(reply)
    return re.sub(r'\bi=\d+', 'i=*', reply) if 'I=' in reply.partition(';')[0] else reply


def matches(expected: dict[str, Any], actual: dict[str, Any]) -> bool:
    got = [norm(r) for r in actual.get('replies', [])]
    want = expected['replies']
    return (len(got) == len(want)
            and all(fnmatch.fnmatchcase(r, p) for r, p in zip(got, want))
            and all(actual.get(k) == expected[k] for k in ('cursor', 'files')))


def check(paw: str) -> int:
    golden = json.loads(GOLDEN.read_text())
    actual = launch([paw, '--config=/dev/null', '--window-size-chars=80x24'])
    failed = 0
    for name, case in golden['cases'].items():
        expected = dict(case, replies=[pattern(r) for r in case['replies']])
        expected.update(KNOWN_DIFFS.get(name, {}))
        got = actual.get(name, {})
        if not matches(expected, got):
            failed += 1
            print(f'FAIL {name}\n  kitty: {json.dumps(expected)}\n  paw:   {json.dumps(got)}')
    print(f'{len(golden["cases"]) - failed}/{len(golden["cases"])} cases match kitty')
    return 1 if failed else 0


def record() -> None:
    version = subprocess.run(['kitty', '--version'], capture_output=True,
                             text=True, check=True).stdout.strip()
    cases = launch(['kitty', '--config=NONE', '-o', 'remember_window_size=no',
                    '-o', 'initial_window_width=80c', '-o', 'initial_window_height=24c'])
    GOLDEN.write_text(json.dumps({'terminal': version, 'cases': cases}, indent=1) + '\n')
    print(f'wrote {len(cases)} cases from {version} to {GOLDEN}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    sub.add_parser('record')
    p = sub.add_parser('check')
    p.add_argument('--paw', default=str(SCRIPT.parent.parent / 'bld' / 'paw'))
    p = sub.add_parser('run')
    p.add_argument('out', type=Path)
    args = parser.parse_args()

    if args.cmd == 'run':
        run(args.out)
    elif args.cmd == 'record':
        record()
    else:
        sys.exit(check(args.paw))


if __name__ == '__main__':
    main()
