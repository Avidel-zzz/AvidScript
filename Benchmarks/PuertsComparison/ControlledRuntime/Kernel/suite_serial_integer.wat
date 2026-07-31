(module
  (memory (export "memory") 1)
  (func (export "run") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32) (local $value i32) (local $aux i32)
    local.get $seed
    i32.const 0x6d2b79f5
    i32.xor
    local.set $value
    local.get $seed
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
        i32.const 5
        i32.rotl
        local.get $value
        i32.xor
        i32.const 1103515245
        i32.mul
        i32.const 12345
        i32.add
        local.set $aux
        i32.const 0
        local.get $aux
        i32.store
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    local.get $value))
