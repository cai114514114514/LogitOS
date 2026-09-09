// 20000 sequential awaits in one async function: a framework's async data
// pipeline. The engine must not grow the C stack per await.
(async function () {
  var n = 0;
  for (var i = 0; i < 20000; i++) n = await (n + 1);
  print('awaited', n);
})().catch(function (e) { print('THREW', e.name); });
