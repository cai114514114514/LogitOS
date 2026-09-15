#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional host model service for QEMU; provider credentials stay on the host.

The guest uses an ephemeral capability for this one bounded chat endpoint.
Neither request bodies nor authentication headers are logged. This is also
useful when testing LogitOS without making provider TLS a prerequisite.
"""
import argparse,http.server,json,pathlib,secrets,threading,urllib.request,urllib.error,os,time
p=argparse.ArgumentParser();p.add_argument('--env',type=pathlib.Path,required=True);p.add_argument('--state',type=pathlib.Path,required=True);p.add_argument('--port',type=int,default=0);p.add_argument('--limit',type=int,default=32);a=p.parse_args()
values={}
for line in a.env.read_text().splitlines():
 line=line.strip()
 if not line or line.startswith('#'):continue
 k,sep,v=line.removeprefix('export ').partition('=')
 if sep:values[k.strip().lower()]=v.strip().strip('\"\'')
key=values.get('deepseek_api') or values.get('deepseek_api_key')
if not key:raise SystemExit('No DeepSeek API key field found')
a.state.mkdir(parents=True,exist_ok=True);os.chmod(a.state,0o700)
token=secrets.token_urlsafe(32);lock=threading.Lock();calls=0;active=0;results=[]
def private(name,text):
 path=a.state/name;fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
 with os.fdopen(fd,'w') as f:f.write(text)
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def send(self,status,data):
  self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 def do_POST(self):
  global calls,active
  if self.path!='/chat/completions' or self.headers.get('Authorization')!='Bearer '+token:return self.send(403,b'{"error":"denied"}')
  try:
   n=int(self.headers.get('Content-Length','0'))
   if not 0<n<=24*1024*1024:raise ValueError()
   body=self.rfile.read(n);obj=json.loads(body)
   if obj.get('model')!='deepseek-flash' or obj.get('stream') is not False:raise ValueError()
   if not 0<int(obj.get('max_tokens',0))<=8192:raise ValueError()
  except Exception:return self.send(400,b'{"error":"invalid request"}')
  with lock:
   if calls>=a.limit or active>=2:return self.send(429,b'{"error":"gateway budget"}')
   calls+=1;active+=1;operation=calls
  start=time.monotonic();code=502;result=b'{"error":"provider unavailable"}'
  try:
   req=urllib.request.Request('https://api.deepseek.com/chat/completions',body,headers={'Authorization':'Bearer '+key,'Content-Type':'application/json'})
   with urllib.request.urlopen(req,timeout=150) as f:code=f.status;result=f.read(4*1024*1024+1)
   if len(result)>4*1024*1024:code=502;result=b'{"error":"response limit"}'
  except urllib.error.HTTPError as e:code=e.code
  except Exception:pass
  finally:
   with lock:
    active-=1;results.append(dict(call=operation,http=code,seconds=round(time.monotonic()-start,3)))
    private('metrics.json',json.dumps(dict(provider='DeepSeek',model='deepseek-flash',calls=calls,results=results),indent=2)+'\n')
  try:self.send(code,result)
  except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',a.port),Handler)
private('agent.key',token+'\n')
private('agent.conf',f'host=10.0.2.2\nport={server.server_port}\ntls=0\npath=/chat/completions\nmodel=deepseek-flash\nthinking=0\nmax_tokens=2048\nkey_file=/etc/agent.key\n')
private('ready.json',json.dumps(dict(port=server.server_port,provider='DeepSeek',model='deepseek-flash'))+'\n')
print('GATEWAY_READY model=deepseek-flash port='+str(server.server_port),flush=True)
try:server.serve_forever()
except KeyboardInterrupt:pass
finally:server.server_close()
