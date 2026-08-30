(module
  (import "env" "base" (global $b i32))
  (func (export "get") (result i32) (global.get $b)))
