(module
  (memory (export "mem") 1 4)
  (func (export "poke") (param i32 i32) (i32.store (local.get 0) (local.get 1)))
  (func (export "peek") (param i32) (result i32) (i32.load (local.get 0)))
  (func (export "growit") (param i32) (result i32) (memory.grow (local.get 0))))
