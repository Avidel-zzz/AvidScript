(module
  (memory (export "memory") 1)
  (func $mix (param $value i32) (param $index i32) (result i32)
    local.get $value
    local.get $index
    i32.xor
    i32.const 7
    i32.rotl
    i32.const 0x9e3779b9
    i32.add)
  (func (export "run") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32) (local $value i32) (local $aux i32)
    local.get $seed
    i32.const 0x6d2b79f5
    i32.xor
    local.set $value
    block $done
      local.get $iterations
      i32.eqz
      br_if $done
      loop $next
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
        local.get $value
        local.get $index
        call $mix
        i32.xor
        local.set $aux
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        local.get $index
        local.get $iterations
        i32.lt_u
        br_if $next
      end
    end
    i32.const 0
    local.get $aux
    i32.store
    local.get $value))
