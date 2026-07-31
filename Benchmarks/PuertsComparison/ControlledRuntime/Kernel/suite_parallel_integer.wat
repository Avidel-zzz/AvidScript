(module
  (memory (export "memory") 1)
  (func (export "run") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32) (local $value i32)
    (local $a i32) (local $b i32) (local $c i32) (local $d i32)
    local.get $seed
    i32.const 0x6d2b79f5
    i32.xor
    local.set $value
    local.get $seed
    local.tee $a
    i32.const 0x9e3779b9
    i32.xor
    local.tee $b
    i32.const 0x7f4a7c15
    i32.xor
    local.tee $c
    i32.const 0x51ed270b
    i32.xor
    local.set $d
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
        local.get $a
        i32.const 3
        i32.rotl
        i32.const 33
        i32.add
        local.set $a
        local.get $b
        i32.const 5
        i32.rotl
        i32.const 65
        i32.add
        local.set $b
        local.get $c
        i32.const 7
        i32.rotl
        i32.const 129
        i32.add
        local.set $c
        local.get $d
        i32.const 11
        i32.rotl
        i32.const 257
        i32.add
        local.set $d
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    i32.const 0
    local.get $a
    i32.store
    i32.const 4
    local.get $b
    i32.store
    i32.const 8
    local.get $c
    i32.store
    i32.const 12
    local.get $d
    i32.store
    local.get $value))
