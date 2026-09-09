// A thenable costs EXTRA turns: the job that calls its .then is itself a job.
var thenable = { then: function (res) { print('thenable.then called'); res('T'); } };
Promise.resolve().then(function(){print('t0');});
Promise.resolve(thenable).then(function(v){print('thenable resolved', v);});
Promise.resolve().then(function(){print('t1');}).then(function(){print('t2');})
  .then(function(){print('t3');}).then(function(){print('t4');});
print('sync');
