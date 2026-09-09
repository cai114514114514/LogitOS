Promise.any([Promise.reject(new Error('a')), Promise.reject(new Error('b'))])
  .catch(function (e) { print('name', e.name, 'errors', e.errors.length, e.errors.map(function(x){return x.message;}).join(',')); });
Promise.any([]).catch(function (e) { print('empty', e.name, e.errors.length); });
