(module (func (export "half") (param f32) (result f32)
  (f32.div (local.get 0) (f32.const 2))))
