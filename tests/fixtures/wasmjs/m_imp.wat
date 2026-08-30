(module
  (import "env" "twice" (func $twice (param i32) (result i32)))
  (func (export "call4") (result i32) (call $twice (i32.const 4))))
