// The executor is SYNCHRONOUS; resolving late does not change that. This is
// the shape of `document.currentScript` read from a promise reaction: what is
// synchronous and what is deferred.
var seen = 'set-at-top';
new Promise(function (res) { print('executor sees', seen); res(); }).then(function () {
  print('reaction sees', seen);
});
Promise.resolve().then(function(){ print('reaction2 sees', seen); });
seen = 'changed-before-microtasks';
print('sync sees', seen);
