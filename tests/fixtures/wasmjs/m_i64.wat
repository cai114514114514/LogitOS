(module (func (export "inc") (param i64) (result i64)
  (i64.add (local.get 0) (i64.const 1))))
