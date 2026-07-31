(module
  (memory (export "memory") 1)
  (func (export "run") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32) (local $value i32) (local $aux f64)
    local.get $seed
    i32.const 0x6d2b79f5
    i32.xor
    local.set $value
    f64.const 1.0
    local.set $aux
    block $done
      loop $next
        local.get $index
        local.get $iterations
        i32.ge_u
        br_if $done
        local.get $value
        i32.const 13
        i32.rotl
        i32.const 1664525
        i32.mul
        i32.const 1013904223
        i32.add
        local.get $seed
        local.get $index
        i32.add
        i32.xor
        local.set $value
        local.get $aux
        f64.const 1.00000011920928955078125
        f64.mul
        local.get $value
        f64.convert_i32_s
        f64.const 0.00000095367431640625
        f64.mul
        f64.add
        local.set $aux
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    i32.const 0
    local.get $aux
    f64.store
    local.get $value))
