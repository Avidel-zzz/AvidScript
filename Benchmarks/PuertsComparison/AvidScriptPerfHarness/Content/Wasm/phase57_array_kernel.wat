(module
  (import "avidscript" "avid_value_array_load"
    (func $array_load (param i32 i32 i32 i32) (result i32)))
  (import "avidscript" "avid_value_array_store"
    (func $array_store (param i32 i32 i32 i32) (result i32)))
  (import "avidscript" "avid_value_array_read_range"
    (func $array_read_range (param i32 i32 i32 i32 i32) (result i32)))
  (import "avidscript" "avid_value_array_write_range"
    (func $array_write_range (param i32 i32 i32 i32 i32) (result i32)))

  (memory (export "memory") 1)

  ;; 0: capability token, 4: logical calls, 16: element scratch, 64: final hash,
  ;; 1024: managed-array length header, 1028: managed-array payload.
  (func $mix (param $value i32) (result i32)
    local.get $value
    i32.const 1664525
    i32.mul
    i32.const 1013904223
    i32.add)

  (func $element_pass (param $size i32)
    (local $token i32)
    (local $index i32)
    (local $value i32)
    (local $hash i32)
    i32.const 0
    i32.load
    local.set $token
    i32.const -2128831035
    local.set $hash
    i32.const 0
    local.set $index
    block $done
      loop $next
        local.get $index
        local.get $size
        i32.ge_u
        br_if $done
        local.get $token
        local.get $index
        i32.const 16
        i32.const 4
        call $array_load
        i32.eqz
        if
          unreachable
        end
        i32.const 16
        i32.load
        local.get $index
        i32.xor
        call $mix
        local.set $value
        i32.const 16
        local.get $value
        i32.store
        local.get $token
        local.get $index
        i32.const 16
        i32.const 4
        call $array_store
        i32.eqz
        if
          unreachable
        end
        local.get $hash
        local.get $value
        i32.xor
        i32.const 16777619
        i32.mul
        local.set $hash
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    i32.const 64
    local.get $hash
    i32.store)

  (func $bulk_pass (param $size i32)
    (local $token i32)
    (local $index i32)
    (local $address i32)
    (local $value i32)
    (local $hash i32)
    i32.const 0
    i32.load
    local.set $token
    i32.const 1024
    local.get $size
    i32.store
    local.get $token
    i32.const 0
    i32.const 1024
    i32.const 0
    local.get $size
    call $array_read_range
    i32.eqz
    if
      unreachable
    end
    i32.const -2128831035
    local.set $hash
    i32.const 0
    local.set $index
    block $done
      loop $next
        local.get $index
        local.get $size
        i32.ge_u
        br_if $done
        local.get $index
        i32.const 2
        i32.shl
        i32.const 1028
        i32.add
        local.tee $address
        i32.load
        local.get $index
        i32.xor
        call $mix
        local.set $value
        local.get $address
        local.get $value
        i32.store
        local.get $hash
        local.get $value
        i32.xor
        i32.const 16777619
        i32.mul
        local.set $hash
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end
    local.get $token
    i32.const 0
    i32.const 1024
    i32.const 0
    local.get $size
    call $array_write_range
    i32.eqz
    if
      unreachable
    end
    i32.const 64
    local.get $hash
    i32.store)

  (func (export "avid_on_begin_play"))
  (func (export "avid_on_tick") (param f32))
  (func (export "avid_on_end_play"))
  (func (export "avid_on_event") (param $mode i32) (param $size f32)
    (local $remaining i32)
    i32.const 4
    i32.load
    local.set $remaining
    block $done
      loop $next
        local.get $remaining
        i32.eqz
        br_if $done
        local.get $mode
        i32.eqz
        if
          local.get $size
          i32.trunc_f32_u
          call $element_pass
        else
          local.get $mode
          i32.const 1
          i32.eq
          if
            local.get $size
            i32.trunc_f32_u
            call $bulk_pass
          end
        end
        local.get $remaining
        i32.const 1
        i32.sub
        local.set $remaining
        br $next
      end
    end
  )
)
