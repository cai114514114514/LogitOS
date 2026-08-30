(module
  (import "env" "m" (memory 1))
  (func (export "store") (param i32 i32) (i32.store (local.get 0) (local.get 1)))
  (func (export "growstore") (param i32) (result i32)
    (drop (memory.grow (i32.const 1)))
    (i32.store (i32.const 0) (local.get 0))
    (memory.size)))
