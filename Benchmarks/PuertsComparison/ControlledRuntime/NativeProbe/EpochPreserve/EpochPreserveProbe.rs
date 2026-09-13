use std::path::PathBuf;
use std::sync::{Arc, atomic::{AtomicBool, Ordering}};
use std::time::Duration;
use wasmtime::*;

#[inline(never)]
fn clobber_host_volatile_registers() {
    // Deliberately destroy all Windows host-volatile GPRs and XMM registers.
    // Otherwise a callback with no floating-point work could accidentally
    // leave the caller's live SIMD values intact even with a broken wrapper.
    unsafe {
        core::arch::asm!(
            "mov rax, -1", "mov rcx, -1", "mov rdx, -1",
            "mov r8, -1", "mov r9, -1", "mov r10, -1", "mov r11, -1",
            "pxor xmm0, xmm0", "pxor xmm1, xmm1", "pxor xmm2, xmm2",
            "pxor xmm3, xmm3", "pxor xmm4, xmm4", "pxor xmm5, xmm5",
            out("rax") _, out("rcx") _, out("rdx") _,
            out("r8") _, out("r9") _, out("r10") _, out("r11") _,
            out("xmm0") _, out("xmm1") _, out("xmm2") _,
            out("xmm3") _, out("xmm4") _, out("xmm5") _,
            options(nostack, nomem),
        );
    }
}

fn engine(epoch: bool) -> Result<Engine> {
    let mut c = Config::new();
    c.epoch_interruption(epoch)
        .consume_fuel(false)
        .cranelift_opt_level(OptLevel::Speed)
        .cranelift_regalloc_algorithm(RegallocAlgorithm::Backtracking)
        .compiler_inlining(Inlining::Yes)
        .cranelift_debug_verifier(true);
    Engine::new(&c)
}

fn load(engine: &Engine, bytes: &[u8]) -> Result<(Store<usize>, TypedFunc<(i32, i32), i32>, Option<Memory>)> {
    let module = Module::new(engine, bytes)?;
    let mut store = Store::new(engine, 0usize);
    store.set_epoch_deadline(1);
    let instance = Instance::new(&mut store, &module, &[])?;
    let f = instance.get_typed_func::<(i32, i32), i32>(&mut store, "run")?;
    let memory = instance.get_memory(&mut store, "memory");
    Ok((store, f, memory))
}

fn snapshot(store: &Store<usize>, memory: Option<Memory>) -> Vec<u8> {
    memory.map(|m| m.data(store).to_vec()).unwrap_or_default()
}

fn main() -> Result<()> {
    let done = Arc::new(AtomicBool::new(false));
    let watchdog = done.clone();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(60));
        if !watchdog.load(Ordering::SeqCst) {
            eprintln!("FAIL watchdog: epoch probe exceeded 60 seconds");
            std::process::exit(124);
        }
    });
    let root = PathBuf::from(std::env::args().nth(1).expect("kernel directory required"));
    let output = PathBuf::from(std::env::args().nth(2).expect("artifact directory required"));
    std::fs::create_dir(&output)?;
    let on = engine(true)?;
    let off = engine(false)?;
    let mut passed = 0;
    for name in ["scalar_float", "simd128", "mixed_gameplay", "data_branch"] {
        let bytes = std::fs::read(root.join(format!("suite_{name}.wasm")))?;
        let (mut reference_store, reference, reference_memory) = load(&off, &bytes)?;
        let expected = reference.call(&mut reference_store, (127, 1397313))?;
        let expected_memory = snapshot(&reference_store, reference_memory);
        if matches!(name, "scalar_float" | "simd128") {
            assert!(!expected_memory.is_empty());
            assert!(expected_memory.iter().any(|b| *b != 0));
        }

        // Expiry must trap before executing the export, and the same instance
        // must remain usable after resetting its deadline.
        let (mut store, f, memory) = load(&on, &bytes)?;
        store.set_epoch_deadline(0);
        let error = f.call(&mut store, (127, 1397313)).unwrap_err();
        assert_eq!(error.downcast_ref::<Trap>(), Some(&Trap::Interrupt));
        store.set_epoch_deadline(1);
        assert_eq!(f.call(&mut store, (127, 1397313))?, expected);
        assert_eq!(snapshot(&store, memory), expected_memory);
        passed += 1;
        println!("PASS {name} expired_interrupt_and_recovery");

        // Continue with a nonzero delta must refresh the deadline; exactly one
        // callback is expected even though the function has many backedges.
        let (mut store, f, memory) = load(&on, &bytes)?;
        store.set_epoch_deadline(0);
        store.epoch_deadline_callback(|mut context| {
            clobber_host_volatile_registers();
            *context.data_mut() += 1;
            Ok(UpdateDeadline::Continue(100))
        });
        assert_eq!(f.call(&mut store, (127, 1397313))?, expected);
        assert_eq!(snapshot(&store, memory), expected_memory);
        assert_eq!(*store.data(), 1);
        passed += 1;
        println!("PASS {name} continue_deadline_reload");

        // Delta zero deliberately takes the slow path on every epoch check.
        // These real kernels keep integer, floating-point and SIMD state live.
        let (mut store, f, memory) = load(&on, &bytes)?;
        store.set_epoch_deadline(0);
        store.epoch_deadline_callback(|mut context| {
            clobber_host_volatile_registers();
            *context.data_mut() += 1;
            Ok(UpdateDeadline::Continue(0))
        });
        assert_eq!(f.call(&mut store, (127, 1397313))?, expected);
        assert_eq!(snapshot(&store, memory), expected_memory);
        assert!(*store.data() >= 127, "slow path was not exercised per iteration");
        passed += 1;
        println!("PASS {name} repeated_slowpath_state callbacks={}", store.data());

        // A host callback failure must propagate through the trampoline's
        // original sentinel handling, then a later call must recover.
        let (mut store, f, memory) = load(&on, &bytes)?;
        store.set_epoch_deadline(0);
        store.epoch_deadline_callback(|mut context| {
            clobber_host_volatile_registers();
            *context.data_mut() += 1;
            if *context.data() == 17 { return Err(format_err!("epoch-probe-sentinel")); }
            Ok(UpdateDeadline::Continue(0))
        });
        let error = f.call(&mut store, (127, 1397313)).unwrap_err();
        assert!(format!("{error:#}").contains("epoch-probe-sentinel"));
        assert_eq!(*store.data(), 17);
        store.epoch_deadline_trap();
        store.set_epoch_deadline(1);
        assert_eq!(f.call(&mut store, (127, 1397313))?, expected);
        assert_eq!(snapshot(&store, memory), expected_memory);
        passed += 1;
        println!("PASS {name} callback_error_and_recovery");

        for (suffix, engine) in [("epoch_on", &on), ("epoch_off", &off)] {
            let module = Module::new(engine, &bytes)?;
            std::fs::write(output.join(format!("{name}-{suffix}.cwasm")), module.serialize()?)?;
        }
    }

    // Exercise a pure infinite backedge with no host calls. An independent
    // epoch increment must still interrupt it, not merely function entry.
    let bytes = [0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,7,7,1,3,114,117,110,0,0,10,9,1,7,0,3,64,12,0,11,11];
    let module = Module::new(&on, bytes)?;
    let mut store = Store::new(&on, ());
    store.set_epoch_deadline(1);
    let instance = Instance::new(&mut store, &module, &[])?;
    let f = instance.get_typed_func::<(), ()>(&mut store, "run")?;
    let signal = on.clone();
    let ticker = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(50));
        signal.increment_epoch();
    });
    let error = f.call(&mut store, ()).unwrap_err();
    ticker.join().unwrap();
    assert_eq!(error.downcast_ref::<Trap>(), Some(&Trap::Interrupt));
    passed += 1;
    println!("PASS infinite_backedge_interrupt");
    assert_eq!(passed, 17);
    done.store(true, Ordering::SeqCst);
    println!("Epoch preserve probe: passed={passed}/17 failed=0");
    Ok(())
}
