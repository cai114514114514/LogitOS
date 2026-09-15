#!/usr/bin/env python3
"""Independent cryptography signatures and OpenSSH key encodings, no network."""
import base64, ctypes, pathlib, struct, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519, rsa, padding, utils

lib=ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
P=ctypes.c_char_p; I=ctypes.c_int
lib.ssh_pubkey_supported.argtypes=[P,P,I]
lib.ssh_pubkey_verify.argtypes=[P,P,I,P,I,P,I]
lib.ssh_authkeys_match.argtypes=[P,I,P,I]
lib.ssh_pubkey_ext_info.argtypes=[P,I]
lib.ssh_auth_pubkey_signdata.argtypes=[P,P,P,P,P,I,P,I]
lib.ssh_pubkey_init()
checks=0
def check(ok,label):
    global checks
    if not ok: raise AssertionError(label)
    checks+=1; print('PASS '+label,flush=True)
def string(b): return struct.pack('>I',len(b))+b
def mpint(v): return string(v.to_bytes((v.bit_length()+8)//8,'big'))

cases=[('ssh-ed25519',ed25519.Ed25519PrivateKey.generate(),None)]
for bits,curve,h in [(256,ec.SECP256R1(),hashes.SHA256()),(384,ec.SECP384R1(),hashes.SHA384()),(521,ec.SECP521R1(),hashes.SHA512())]:
    cases.append((f'ecdsa-sha2-nistp{bits}',ec.generate_private_key(curve),h))
for bits in (2048,3072,4096):
    key=rsa.generate_private_key(65537,bits)
    cases.extend([(f'rsa-sha2-{h.digest_size*8}',key,h) for h in (hashes.SHA256(),hashes.SHA512())])
for algorithm,key,h in cases:
    alg=algorithm.encode(); line=key.public_key().public_bytes(serialization.Encoding.OpenSSH,serialization.PublicFormat.OpenSSH)
    blob=base64.b64decode(line.split()[1]); sid=bytes(range(32))
    data=string(sid)+b'\x32'+string(b'server')+string(b'ssh-connection')+string(b'publickey')+b'\x01'+string(alg)+string(blob)
    label=algorithm+(f'/{key.key_size}' if hasattr(key,'key_size') else '')
    check(lib.ssh_pubkey_supported(alg,blob,len(blob))==1,'supported '+label)
    buf=ctypes.create_string_buffer(2048)
    n=lib.ssh_auth_pubkey_signdata(sid,b'server',b'ssh-connection',alg,blob,len(blob),buf,len(buf))
    check(n==len(data) and buf.raw[:n]==data,'RFC 4252 signed bytes '+label)
    if algorithm=='ssh-ed25519': raw=key.sign(data)
    elif algorithm.startswith('ecdsa'):
        r,s=utils.decode_dss_signature(key.sign(data,ec.ECDSA(h)));raw=mpint(r)+mpint(s)
    else: raw=key.sign(data,padding.PKCS1v15(),h)
    signature=string(alg)+string(raw)
    check(lib.ssh_pubkey_verify(alg,blob,len(blob),signature,len(signature),data,len(data))==1,'independent signature '+label)
    authorized=b'# client keys\r\n\t'+line+b' workstation\r\n'
    check(lib.ssh_authkeys_match(authorized,len(authorized),blob,len(blob))==1,'authorized_keys '+label)
    # Ordinary administrator configuration: unsupported restrictions must
    # remain excluded, never turn a restricted key into an unrestricted one.
    restricted=b'restrict '+line+b'\n'
    check(lib.ssh_authkeys_match(restricted,len(restricted),blob,len(blob))==0,'unsupported key options remain excluded '+label)
buf=ctypes.create_string_buffer(256);n=lib.ssh_pubkey_ext_info(buf,len(buf))
expected=b'\x07'+struct.pack('>I',1)+string(b'server-sig-algs')+string(b'ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256')
check(n==len(expected) and buf.raw[:n]==expected,'RFC 8308 server signature advertisement')
print(f'SSH public-key integration: {checks} checks passed')
