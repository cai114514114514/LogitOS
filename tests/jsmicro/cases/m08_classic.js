// The interview puzzle, verbatim, because every framework's flush is this shape.
async function async1() { print('async1 start'); await async2(); print('async1 end'); }
async function async2() { print('async2'); }
print('script start');
async1();
new Promise(function (resolve) { print('promise1'); resolve(); })
  .then(function () { print('then1'); });
print('script end');
