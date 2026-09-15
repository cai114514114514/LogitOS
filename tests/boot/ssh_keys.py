"""Fresh standard client keys exercise the actual guest authentication path."""
import concurrent.futures

def prepare(tmp,key,require):
    cases=[]
    for kind,bits in [('ecdsa',256),('ecdsa',384),('ecdsa',521),('rsa',2048),('rsa',3072),('rsa',4096)]:
        path=tmp/f'client-{kind}-{bits}'
        require(['ssh-keygen','-q','-t',kind,'-b',str(bits),'-N','','-f',str(path)])
        algorithms=[f'ecdsa-sha2-nistp{bits}'] if kind=='ecdsa' else ['rsa-sha2-256','rsa-sha2-512']
        cases.extend((algorithm,bits,path) for algorithm in algorithms)
    authorized=tmp/'authorized_keys'
    # Put new families beyond the previous 4 KiB reader boundary; real users
    # accumulate comments and several workstation keys in this same file.
    authorized.write_text(key.with_suffix('.pub').read_text()+'# reserved workstation slot\n'*170+
        ''.join(path.with_suffix('.pub').read_text() for path in dict.fromkeys(c[2] for c in cases)))
    return authorized,cases

def exercise(cases,sshbase,run,check,out):
    def command(case,command,stdin=b''):
        alg,bits,key=case
        args=list(sshbase);args[args.index('-i')+1]=str(key)
        args=args[:-1]+['-o','BatchMode=yes','-o','PreferredAuthentications=publickey',
            '-o','PubkeyAcceptedAlgorithms='+alg,'-vv',args[-1],command]
        r=run(args,input=stdin,timeout=120)
        (out/f'key-{alg}-{bits}.log').write_bytes(r.stderr)
        return r
    for case in cases:
        alg,bits,_=case;r=command(case,'echo KEY_LOGIN_OK')
        check(r.returncode==0 and r.stdout.strip()==b'KEY_LOGIN_OK',f'OpenSSH public-key login {alg}/{bits}')
    check(b'server-sig-algs=' in r.stderr and b'rsa-sha2-512' in r.stderr,'OpenSSH receives server-sig-algs after initial NEWKEYS')
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        results=list(pool.map(lambda c:command(c,'echo CONCURRENT_KEYS_OK'),cases[:3]))
    check(all(r.returncode==0 and r.stdout.strip()==b'CONCURRENT_KEYS_OK' for r in results),'concurrent P-256 P-384 P-521 authentication')
    data=bytes(range(256))*2049+b'RSA-file-consumer'
    r=command(cases[-1],'cat > /home/server/rsa-transfer.bin',data)
    q=command(cases[-1],'cat /home/server/rsa-transfer.bin')
    check(r.returncode==0 and q.returncode==0 and q.stdout==data,'RSA-4096 SHA-512 authenticated file transfer exact bytes')

def verify_reboot(cases,sshbase,run,check,out):
    alg,bits,key=cases[-1];args=list(sshbase);args[args.index('-i')+1]=str(key)
    r=run(args[:-1]+['-o','BatchMode=yes','-o','PreferredAuthentications=publickey',
        '-o','PubkeyAcceptedAlgorithms='+alg,args[-1],'echo RSA_REBOOT_OK'])
    (out/'key-reboot.log').write_bytes(r.stderr)
    check(r.returncode==0 and r.stdout.strip()==b'RSA_REBOOT_OK','RSA-4096 authorization survives product rebuild and reboot')
