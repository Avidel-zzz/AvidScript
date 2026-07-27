(module
  (memory (export "memory") 1)
  (func (export "run") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32) (local $value i32) (local $address i32) (local $aux i32)
    local.get $seed
    i32.const 0x6d2b79f5
    i32.xor
    local.set $value
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
        local.get $index
        i32.const 255
        i32.and
        i32.const 4
        i32.shl
        local.set $address
        local.get $address
        local.get $value
        i32.store
        local.get $address
        local.get $index
        i32.store offset=4
        local.get $address
        local.get $seed
        i32.store offset=8
        local.get $address
        local.get $value
        local.get $index
        i32.xor
        i32.store offset=12
        local.get $aux
        local.get $address
        i32.load
        local.get $address
        i32.load offset=12
        i32.xor
        i32.add
        local.set $aux
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    i32.const 4096
    local.get $aux
    i32.store
    local.get $value))
