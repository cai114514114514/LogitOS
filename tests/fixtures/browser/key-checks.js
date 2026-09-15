/* SPDX-License-Identifier: MIT */
(async function(){
 const s=crypto.subtle,results=[];globalThis.cryptoKeyResults=results;
 const hx=t=>new Uint8Array(t.match(/../g)?.map(x=>parseInt(x,16))||[]);
 const hex=b=>Array.from(new Uint8Array(b)).map(x=>x.toString(16).padStart(2,'0')).join('');
 const ck=(name,v)=>results.push([name,!!v]);
 async function rejects(name,fn,want){try{await fn();ck(name,false)}catch(e){ck(name,!want||e.name===want)}}
 try{
 for(const row of keyVectors){const name=JSON.stringify(row.alg);
  if(row.signature){
   const pub=await s.importKey('jwk',row.pub,row.alg,true,['verify']);
   const priv=await s.importKey('jwk',row.priv,row.alg,true,['sign']);
   ck(name+' independent signature',await s.verify(row.op,pub,hx(row.signature),hx(row.msg)));
   const sig=await s.sign(row.op,priv,hx(row.msg));ck(name+' native sign',await s.verify(row.op,pub,sig,hx(row.msg)));
   if(row.alg.name==='Ed25519')ck('Ed25519 deterministic signature',hex(sig)===row.signature);
   const corrupt=new Uint8Array(sig);corrupt[0]^=1;ck(name+' changed signature',!(await s.verify(row.op,pub,corrupt,hx(row.msg))));
   const p2=await s.importKey('spki',hx(row.spki),row.alg,true,['verify']);
   const k2=await s.importKey('pkcs8',hx(row.pkcs8),row.alg,true,['sign']);
   ck(name+' independent DER imports',await s.verify(row.op,p2,await s.sign(row.op,k2,hx(row.msg)),hx(row.msg)));
   ck(name+' SPKI exact bytes',hex(await s.exportKey('spki',pub))===row.spki);
   const ourDer=await s.exportKey('pkcs8',priv);const k3=await s.importKey('pkcs8',ourDer,row.alg,true,['sign']);
   ck(name+' PKCS8 usable',await s.verify(row.op,pub,await s.sign(row.op,k3,hx(row.msg)),hx(row.msg)));
   const raw=await s.exportKey('raw',pub);ck(name+' raw public width',raw.byteLength===(row.alg.name==='Ed25519'?32:row.alg.namedCurve==='P-256'?65:row.alg.namedCurve==='P-384'?97:133));
   const kp=await s.generateKey(row.alg,false,['sign','verify']);
   ck(name+' generated key works',await s.verify(row.op,kp.publicKey,await s.sign(row.op,kp.privateKey,hx(row.msg)),hx(row.msg)));
   await rejects(name+' nonextractable',()=>s.exportKey('jwk',kp.privateKey),'InvalidAccessError');
   kp.privateKey.usages.push('verify');await rejects(name+' immutable usage',()=>s.verify(row.op,kp.privateKey,sig,hx(row.msg)),'InvalidAccessError');
   await rejects(name+' invalid usage',()=>s.generateKey(row.alg,true,['encrypt']),'SyntaxError');
   const bad=new Uint8Array(hx(row.spki).length+1);bad.set(hx(row.spki));await rejects(name+' DER trailing bytes',()=>s.importKey('spki',bad,row.alg,true,['verify']),'DataError');
  }else{
   const pub=await s.importKey('jwk',row.pub,row.alg,true,[]),priv=await s.importKey('jwk',row.priv,row.alg,false,['deriveBits','deriveKey']);
   const op={name:row.alg.name,public:pub};ck(name+' independent shared secret',hex(await s.deriveBits(op,priv,null))===row.shared);
   ck(name+' partial bit length',new Uint8Array(await s.deriveBits(op,priv,13))[1]%8===0);
   const aes=await s.deriveKey(op,priv,{name:'AES-GCM',length:128},true,['encrypt','decrypt']);
   ck(name+' derive AES consumer',hex(await s.exportKey('raw',aes))===row.shared.slice(0,32));
   await rejects(name+' overlong secret',()=>s.deriveBits(op,priv,row.shared.length*4+1),'OperationError');
   const a=await s.generateKey(row.alg,true,['deriveBits']),b=await s.generateKey(row.alg,true,['deriveBits']);
   ck(name+' generated pair',hex(await s.deriveBits({name:row.alg.name,public:b.publicKey},a.privateKey,null))===hex(await s.deriveBits({name:row.alg.name,public:a.publicKey},b.privateKey,null)));
  }
 }
 for(const bits of [128,192,256]){
  const key=await s.generateKey({name:'AES-GCM',length:bits},false,['encrypt','decrypt']);const iv=new Uint8Array(12),data=hx('0011223344556677');
  const encrypted=await s.encrypt({name:'AES-GCM',iv},key,data);ck('generated AES '+bits,hex(await s.decrypt({name:'AES-GCM',iv},key,encrypted))===hex(data));
 }
 const h=await s.generateKey({name:'HMAC',hash:'SHA-256'},false,['sign','verify']);const message=hx('123456');
 ck('generated HMAC',await s.verify('HMAC',h,await s.sign('HMAC',h,message),message));
 const alice=await s.generateKey('X25519',true,['deriveBits']);const zero=await s.importKey('raw',new Uint8Array(32),'X25519',true,[]);
 await rejects('zero X25519 shared secret',()=>s.deriveBits({name:'X25519',public:zero},alice.privateKey,256),'OperationError');
 }catch(e){ck('unexpected '+e.name+': '+e.message,false)}
 globalThis.cryptoKeyDone=true;
 if(typeof document!=='undefined'){const out=document.getElementById('key-results');if(out)out.textContent=JSON.stringify({checks:results.length,failed:results.filter(x=>!x[1])})}
 if(typeof console!=='undefined')console.log('KEY_RESULTS '+JSON.stringify({checks:results.length,failed:results.filter(x=>!x[1])}));
})();
