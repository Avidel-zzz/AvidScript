(module
  (memory (export "memory") 1)
  (func (export "run") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32) (local $value i32)
    (local $x f32) (local $y f32) (local $velocity f32) (local $address i32)
    local.get $seed
    i32.const 0x6d2b79f5
    i32.xor
    local.set $value
    local.get $seed
    f32.convert_i32_s
    f32.const 0.0009765625
    f32.mul
    local.set $x
    f32.const 0.0
    local.set $y
    f32.const 0.01666666753590107
    local.set $velocity
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
        local.get $x
        local.get $velocity
        f32.add
        local.set $x
        local.get $y
        local.get $value
        f32.convert_i32_s
        f32.const 0.000000059604644775390625
        f32.mul
        f32.add
        local.set $y
        local.get $value
        i32.const 15
        i32.and
        i32.eqz
        if
          local.get $velocity
          f32.neg
          local.set $velocity
        end
        local.get $index
        i32.const 255
        i32.and
        i32.const 3
        i32.shl
        local.set $address
        local.get $address
        local.get $x
        f32.store
        local.get $address
        local.get $y
        f32.store offset=4
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    i32.const 4096
    local.get $velocity
    f32.store
    local.get $value))
