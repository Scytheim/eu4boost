"""Checks a code-signed eu4boost.dll before a release (tools/release.sh with EB_SIGNED_DLL):

    python tools/pe_check.py <signed dll> <local build> <version>

1. the signed file carries an embedded Authenticode signature (a PKCS#7 entry in the PE certificate table),
2. its product version (version resource, src/version.rc) is <version>,
3. apart from the signature it is the local build of the same commit: the two files are equal once the link
   time stamp, the checksum and the certificate table are left out,
4. on Windows: Windows itself accepts the signature (Get-AuthenticodeSignature says "Valid").
Every check prints a line; the exit code is 0 only if all of them hold.
EB_SKIP_SAME=1 turns check 3 into a warning (if the build on GitHub ever differs for a known reason)."""
import os, struct, subprocess, sys


def pe_fields(d):
    """offsets of the fields that signing and linking change: time stamp, checksum, certificate table entry"""
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    assert d[:2] == b'MZ' and d[pe:pe + 4] == b'PE\0\0', 'not a PE file'
    opt = pe + 24
    assert struct.unpack_from('<H', d, opt)[0] == 0x20b, 'not a 64-bit PE file'
    return pe + 8, opt + 64, opt + 112 + 4 * 8


def cert_table(d):
    """(file offset, size) of the certificate table, (0, 0) if the file is not signed"""
    return struct.unpack_from('<II', d, pe_fields(d)[2])


def has_signature(d):
    off, size = cert_table(d)
    if not off or size < 8 or off + size > len(d):
        return False
    length, revision, kind = struct.unpack_from('<IHH', d, off)
    return 8 < length <= size and revision == 0x0200 and kind == 0x0002     # WIN_CERT_TYPE_PKCS_SIGNED_DATA


def unsigned_part(d):
    """the file without what signing adds or changes, and without the link time stamp"""
    stamp, checksum, sec = pe_fields(d)
    off = cert_table(d)[0]
    b = bytearray(d[:off] if off else d)
    b[stamp:stamp + 4] = b[checksum:checksum + 4] = bytes(4)
    b[sec:sec + 8] = bytes(8)
    return bytes(b)


def same_code(signed, local):
    a, b = unsigned_part(signed), unsigned_part(local)
    if len(a) < len(b) or len(a) - len(b) >= 8 or any(a[len(b):]):          # signing pads the file to 8 bytes
        return False, 'sizes %d and %d' % (len(a), len(b))
    diff = sum(x != y for x, y in zip(a, b))
    return diff == 0, '%d bytes differ' % diff if diff else 'equal'


def product_version(d):
    key = 'ProductVersion'.encode('utf-16-le')
    i = d.find(key)
    if i < 0:
        return None
    j = i + len(key)
    while d[j:j + 2] == b'\0\0':
        j += 2
    k = j
    while k + 2 <= len(d) and d[k:k + 2] != b'\0\0':
        k += 2
    return d[j:k].decode('utf-16-le', 'replace')


def windows_says(path):
    env = dict(os.environ, EB_PE_CHECK_FILE=os.path.abspath(path))
    cmd = '$s = Get-AuthenticodeSignature -LiteralPath $env:EB_PE_CHECK_FILE; $s.Status; $s.SignerCertificate.Subject'
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', cmd], env=env, capture_output=True, text=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    return (lines[0] if lines else 'no answer'), (lines[1] if len(lines) > 1 else '')


def main(signed_path, local_path, version):
    signed, local = open(signed_path, 'rb').read(), open(local_path, 'rb').read()
    bad = 0

    def line(ok, what, warn=False):
        nonlocal bad
        print('%-4s %s' % ('ok' if ok else 'warn' if warn else 'FAIL', what))
        bad += not ok and not warn

    line(has_signature(signed), 'embedded Authenticode signature in %s' % signed_path)
    line(not has_signature(local), 'the local build %s is not signed' % local_path)
    v = product_version(signed)
    line(v == version, 'product version %r (wanted %r)' % (v, version))
    same, how = same_code(signed, local)
    line(same, 'same code as the local build: %s' % how, warn=bool(os.environ.get('EB_SKIP_SAME')))
    if os.name == 'nt':
        status, subject = windows_says(signed_path)
        line(status == 'Valid', 'Windows: signature %s, signer %s' % (status, subject or '(none)'))
    else:
        print('     (not on Windows: the signature itself was not verified)')
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
