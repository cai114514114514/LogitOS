importScripts('clone.js','streams.js','timers.js');
(async function () {
    var pass=0,fail=0,records=[];
    function check(condition,label){if(condition)pass++;else fail++;records.push((condition?'PASS ':'FAIL ')+label);}
    try {await testHTMLClone(check);await testHTMLStreams(check);await testHTMLTimers(check);}
    catch(error){check(false,'Worker exception: '+error.name+': '+error.message);}
    postMessage({kind:'html-priority-worker',pass:pass,fail:fail,records:records});
})();
