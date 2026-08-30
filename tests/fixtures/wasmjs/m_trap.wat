(module
  (func (export "boom") (result i32) unreachable)
  (func (export "divz") (param i32) (result i32)
    (i32.div_s (i32.const 1) (local.get 0))))
