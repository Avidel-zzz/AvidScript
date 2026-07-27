(module
  (import "avidscript" "typed_empty_i32" (func $typed_empty_i32 (result i32)))
  (import "avidscript" "i32_pair" (func $i32_pair (param i32 i32) (result i32)))

  (func (export "run_cached") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32)
    (local $value i32)
    local.get $seed
    local.set $value
    block $done
      loop $next
        local.get $index
        local.get $iterations
        i32.ge_u
        br_if $done
        local.get $value
        local.get $index
        i32.add
        local.set $value
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    local.get $value
  )

  (func (export "run_empty") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32)
    (local $value i32)
    local.get $seed
    local.set $value
    block $done
      loop $next
        local.get $index
        local.get $iterations
        i32.ge_u
        br_if $done
        call $typed_empty_i32
        local.get $value
        i32.xor
        local.get $index
        i32.add
        local.set $value
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    local.get $value
  )

  (func (export "run_i32_pair") (param $iterations i32) (param $seed i32) (result i32)
    (local $index i32)
    (local $value i32)
    local.get $seed
    local.set $value
    block $done
      loop $next
        local.get $index
        local.get $iterations
        i32.ge_u
        br_if $done
        local.get $value
        local.get $seed
        local.get $index
        i32.add
        call $i32_pair
        local.set $value
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    local.get $value
  )
)
