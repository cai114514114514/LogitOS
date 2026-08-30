(module
  (type $t (func (param i32) (result i32)))
  (table (export "tbl") 4 funcref)
  (elem (i32.const 0) $a $b)
  (func $a (type $t) (i32.add (local.get 0) (i32.const 10)))
  (func $b (type $t) (i32.mul (local.get 0) (i32.const 3)))
  (func (export "callidx") (param i32 i32) (result i32)
    (call_indirect (type $t) (local.get 1) (local.get 0))))
