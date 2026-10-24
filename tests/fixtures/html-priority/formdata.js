/* Ordinary public API scenarios. These are deferred acceptance cases, not
 * evidence that the current implementation has already passed them. */
async function testHTMLFormData(check) {
    var form=document.createElement('form');
    form.id='entry-list-form';
    form.innerHTML='<input name="q" value="initial">'+
        '<input name="repeat" value="one"><input name="repeat" value="two">'+
        '<input type="checkbox" name="unchecked"><input type="checkbox" name="checked" checked>'+
        '<select name="choices" multiple><option selected value="a">A</option>'+
        '<optgroup disabled><option selected value="b">B</option></optgroup>'+
        '<option selected value="c">C</option></select>'+
        '<fieldset disabled><legend><input name="legend" value="allowed"></legend>'+
        '<input name="disabled" value="omitted"></fieldset>'+
        '<input type="file" name="upload">'+
        '<input type="hidden" name="_charset_" value="wrong">'+
        '<button name="submitter" value="chosen">Send</button>';
    var external=document.createElement('input');
    external.name='external';external.value='outside';external.setAttribute('form',form.id);
    document.body.appendChild(form);document.body.appendChild(external);
    try {
        form.querySelector('[name=q]').value='current';
        var submitter=form.querySelector('button'), data=new FormData(form,submitter);
        check(data.get('q')==='current','FormData takes current native value');
        check(data.getAll('repeat').join(',')==='one,two','duplicate controls retain tree order');
        check(!data.has('unchecked')&&data.get('checked')==='on','checkbox successful controls');
        check(data.getAll('choices').join(',')==='a,c','multiple select omits disabled optgroup');
        check(data.get('legend')==='allowed'&&!data.has('disabled'),'disabled fieldset first legend exception');
        check(data.get('external')==='outside','external form-associated control');
        check(data.get('submitter')==='chosen','explicit submitter contributes data');
        check(data.get('_charset_')==='UTF-8','hidden charset entry uses actual encoding');
        var directional=document.createElement('section');
        directional.setAttribute('dir','auto');
        directional.innerHTML='<bdi>ignored Latin</bdi><span>\u05d0</span>'+
            '<input name="direction" dirname="direction.dir" value="Latin">';
        form.appendChild(directional);
        check(new FormData(form).get('direction.dir')==='rtl','dirname follows ancestor auto direction excluding isolated text');
        var file=data.get('upload');
        check(file instanceof File&&file.name===''&&file.size===0&&file.type==='application/octet-stream',
            'unselected file input contributes specified empty File');

        var savedEventData, trusted=false, reentrant=false;
        function edit(event) {
            trusted=event.isTrusted&&event instanceof FormDataEvent;
            savedEventData=event.formData;event.formData.set('q','listener');
            try {new FormData(form);} catch(e) {reentrant=e.name==='InvalidStateError';}
        }
        form.addEventListener('formdata',edit);
        var changed=new FormData(form);
        form.removeEventListener('formdata',edit);
        check(trusted,'formdata uses a real trusted FormDataEvent');
        check(reentrant,'recursive entry construction refuses before another event');
        check(changed.get('q')==='listener','formdata listener edits reach constructor result');
        savedEventData.set('q','later');
        check(changed.get('q')==='listener','event FormData and constructor result have separate entry lists');
        var events=0;
        function submit(event){events++;check(event.submitter===submitter,'SubmitEvent retains real submitter');event.preventDefault();}
        form.addEventListener('submit',submit);
        submitter.click();form.removeEventListener('submit',submit);
        check(events===1,'scripted button click enters common submit controller');
    } finally {form.remove();external.remove();}

    var live=new FormData();live.append('a','first');
    var iterator=live.entries();check(iterator.next().value[0]==='a','FormData iterator first entry');
    live.append('b','second');check(iterator.next().value[0]==='b','FormData iterator visits later append');
    live.append('b','third');live.set('b','replacement');
    check(live.getAll('b').join(',')==='replacement','set replaces first duplicate in place and removes rest');
    var visits=[];
    live.forEach(function(value,name){visits.push(name);if(name==='a')live.append('c','third');});
    check(visits.join(',')==='a,b,c','forEach follows live appended entries');
    live._e=[['forged','payload']];
    var encoded=await new Response(live).text();
    check(encoded.indexOf('name="a"')>=0&&encoded.indexOf('forged')<0,'Fetch consumes private FormData entries');
    live.append('\ud800','\udc00');
    check(live.get('\ufffd')==='\ufffd','FormData names and values use scalar value strings');

    var bytes=new Uint8Array([65,66]), blob=new Blob([bytes],{type:'APPLICATION/Custom'});
    bytes[0]=90;
    var diagnostic=blob._b;diagnostic[0]=88;
    check((await blob.bytes())[0]===65&&blob.size===2,'Blob payload survives caller and public diagnostic writes');
    check(blob.type==='application/custom','Blob type uses printable ASCII lowercase');
    var sliced=blob.slice(-1);
    check(sliced.size===1&&(await sliced.bytes())[0]===66,'Blob slice owns the requested real byte range');
    var objectURL=URL.createObjectURL(blob);
    try {check(await (await fetch(objectURL)).text()==='AB','object URL Fetch reads the same private Blob bytes');}
    finally {URL.revokeObjectURL(objectURL);}
    var bad=false;
    try {FormData.prototype.append.call({},'x','y');} catch(e){bad=e instanceof TypeError;}
    check(bad,'FormData method refuses forged receiver');
    bad=false;try{new Blob({length:0});}catch(e){bad=e instanceof TypeError;}
    check(bad,'Blob sequence conversion refuses non-iterable parts');
}
