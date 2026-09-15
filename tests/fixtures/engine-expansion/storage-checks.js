/* This fixture deliberately performs no write at load time. Reopening the
 * page must observe disk state rather than recreate the expected marker. */
(function(){
var key='logitos-persistence-marker', sessionKey='logitos-session-marker';
function render(prefix){try{
 var local=localStorage.getItem(key),session=sessionStorage.getItem(sessionKey);
 document.getElementById('storage-result').textContent=prefix+'\nLOCAL: '+(local===null?'absent':local)+'\nSESSION: '+(session===null?'absent':session);
}catch(e){document.getElementById('storage-result').textContent='ERROR '+e.name+': '+e.message}}
var write=document.getElementById('storage-write'),read=document.getElementById('storage-read'),clear=document.getElementById('storage-clear');
if(write)write.onclick=function(){try{var token='guest-'+Date.now();localStorage.setItem(key,token);sessionStorage.setItem(sessionKey,'tab-only');render('WRITE COMMITTED')}catch(e){document.getElementById('storage-result').textContent='WRITE ERROR '+e.name+': '+e.message}};
if(read)read.onclick=function(){render('READ ONLY')};
if(clear)clear.onclick=function(){try{localStorage.removeItem(key);sessionStorage.removeItem(sessionKey);render('CLEAR COMMITTED')}catch(e){document.getElementById('storage-result').textContent='CLEAR ERROR '+e.name+': '+e.message}};
render('PAGE OPEN: READ ONLY');
})();
