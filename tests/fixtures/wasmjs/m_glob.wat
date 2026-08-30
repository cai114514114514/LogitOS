(module
  (global $g (export "g") (mut i32) (i32.const 7))
  (global $c (export "c") i32 (i32.const 99))
  (func (export "bump") (result i32)
    (global.set $g (i32.add (global.get $g) (i32.const 1)))
    (global.get $g)))
