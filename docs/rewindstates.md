Directory structure:
└── giufin-giuroll/
    ├── README.md
    ├── build.rs
    ├── Cargo.toml
    ├── giuroll.ini
    ├── LICENSE
    ├── ilhookmod/
    │   ├── Cargo.toml
    │   ├── LICENSE
    │   └── src/
    │       ├── lib.rs
    │       └── x86.rs
    ├── injector/
    │   ├── README.md
    │   ├── Cargo.toml
    │   ├── LICENSE
    │   ├── .gitignore
    │   └── src/
    │       └── main.rs
    ├── mininip/
    │   ├── README.md
    │   ├── bad.ini
    │   ├── Cargo.toml
    │   ├── good.ini
    │   ├── .gitignore
    │   └── src/
    │       ├── lib.rs
    │       ├── tests.rs
    │       ├── datas/
    │       │   ├── mod.rs
    │       │   ├── tests.rs
    │       │   └── tree/
    │       │       ├── mod.rs
    │       │       └── tests.rs
    │       ├── dump/
    │       │   ├── mod.rs
    │       │   ├── tests.rs
    │       │   └── dumper/
    │       │       ├── mod.rs
    │       │       └── tests.rs
    │       ├── errors/
    │       │   ├── mod.rs
    │       │   └── tests.rs
    │       └── parse/
    │           ├── mod.rs
    │           ├── tests.rs
    │           └── parser/
    │               ├── mod.rs
    │               └── tests.rs
    ├── src/
    │   ├── lib.rs
    │   ├── netcode.rs
    │   ├── replay.rs
    │   ├── rollback.rs
    │   └── sound.rs
    └── .github/
        └── workflows/
            └── build.yml

================================================
File: README.md
================================================
# Giuroll  

Is a network rollback mod for 東方非想天則 / Touhou 12.3 Hisoutensoku, which aims to significantly improve the responsiveness of netplay, as well as introducing other rollback related improvements to replay mode.  

Currently this is an early version, and might slightly increase instability, but will still significantly improve the netplay experience for almost all connections.  

This repository also contains a stripped down version version of the crate [ilhook-rs](https://github.com/regomne/ilhook-rs), and a modified version of [mininip](https://github.com/SlooowAndFurious/mininip), all rights remain with their respective authors.

## Usage  

### For [SWRSToys](https://github.com/SokuDev/SokuMods/) users
- navigate to your Hisoutensoku folder;
- You should see a subfolder called `modules`, and a file called `SWRSToys.ini`
- drop the giuroll folder from this zip into `modules`
- add the following line into your `SWRSTOYS.ini`
`giuroll=modules/giuroll/giuroll.dll`

- find the following line
`SWRSokuRoll=modules/SWRSokuRoll/SWRSokuRoll.dll`
- add a `;` at the beginning of that line, making it
`; SWRSokuRoll=modules/SWRSokuRoll/SWRSokuRoll.dll`

### For users without SWRSToys
Mod can be loaded using the [Injector](/injector/).  
The injector needs to be built from source, and placed alongside the dll and the ini, which can be found in official releases.
- Start th123.exe 
- Run the injector  

if sucessfull, you should see a message.  
If the injector closes abruptly, contact me about it.

### More information about the usage in game is available in the `installation and usage.txt` file inside the distributed zip

## Replay rewind  

In replay mode pressing `q` will start rewinding, using keys that modify playback speed (A/S/D) will affect the rewind speed.  
You can also pause the replay by pressing `z`. When the replay is paused this way you can move frame by frame, backwards or forwards, by using `s` and `d`

## Building from source

Mod can be buit with `cargo` using the `nightly-i686-pc-windows-msvc` toolchain.  
When building from source please remember to add the `--release`/`-r` flag.

## Common problems  

- Game doesn't load: check if the ini is valid according to the example ini provided in this repository, and is placed alongside the mod without any changes to it's name, and check for mod conflicts by disabling all other mods, and adding them back one by one.  
- Failed to connect: either player is using an incompatible version of giuroll, or is not using it at all.  
- Game desynced: I'm planning on adding a desync detector to make debugging desyncs easier, but since desyncs also occur with SokuRoll there is no guarantee they are caused solely by the rollback. If the desyncs are common, persists between game restarts, and are not appearing with Sokuroll, you can contact me about it.


you can contact me about any issues through discord: `@giufin` in DMs.

### Special thanks to:

[DPhoenix](https://github.com/enebe-nb) and [PinkySmile](https://github.com/Gegel85) - for advice and support with hisoutensoku modding  

Ysaron - for majority of the testing 

TStar, Barcode, Rouen, ChèvreDeFeu, Klempfer, LunaTriv, Aquatic, BIG BREWED and Sigets - for additional testing

[Slavfox](https://github.com/slavfox) - for various help with reverse engineering and open source

Fireseal - for making the original rollback mod for hisoutensoku



================================================
File: build.rs
================================================
use std::env;

use winres::{VersionInfo, WindowsResource};

extern crate winres;

fn main() {
    let mut res = WindowsResource::new();
    if cfg!(unix) {
        // from https://github.com/mxre/winres/blob/1807bec3552cd2f0d0544420584d6d78be5e3636/example/build.rs#L10
        // ar tool for mingw in toolkit path
        res.set_ar_path("/usr/i686-w64-mingw32/bin/ar");
        // windres tool
        res.set_windres_path("/usr/bin/i686-w64-mingw32-windres");
    }

    if let Some(version_pre) = env::var("CARGO_PKG_VERSION_PRE").unwrap().splitn(2, "-").next() {
        let mut version = 0_u64;
        version |= env::var("CARGO_PKG_VERSION_MAJOR")
            .unwrap()
            .parse()
            .unwrap_or(0)
            << 48;
        version |= env::var("CARGO_PKG_VERSION_MINOR")
            .unwrap()
            .parse()
            .unwrap_or(0)
            << 32;
        version |= env::var("CARGO_PKG_VERSION_PATCH")
            .unwrap()
            .parse()
            .unwrap_or(0)
            << 16;
        version |= version_pre.parse().unwrap_or(0);
        res.set_version_info(VersionInfo::FILEVERSION, version);
        res.set_version_info(VersionInfo::PRODUCTVERSION, version);
    } else {
        panic!();
    }

    if let Err(e) = res.compile() {
        eprintln!("{}", e);
        std::process::exit(1);
    }
}



================================================
File: Cargo.toml
================================================
cargo-features = ["profile-rustflags"]
[package]
name = "giuroll"
authors = ["Giufin"]
license = "MIT"
version = "0.6.14"
edition = "2021"
build = "build.rs"

# See more keys and their definitions at https://doc.rust-lang.org/cargo/reference/manifest.html
[lib]
crate-type = ["cdylib"]

[features]
logtofile = ["dep:fern", "dep:humantime", "dep:log"]
allocconsole = []
f62 = []

[dependencies.windows]
version = "0.48"
features = [
    "Win32_UI_Input_KeyboardAndMouse",
    "Win32_System_Memory",
    "Win32_Networking_WinSock",
    "Win32_Foundation",
    "Win32_UI_WindowsAndMessaging",
    "Win32_System_Threading",
    "Win32_System_Console",
]

[dependencies]
ilhook = { path = "ilhookmod" }
mininip = { path = "mininip" }                              #"1.3.1"
winapi = { version = "0.3.9", features = ["libloaderapi"] }

fern = { version = "0.6.2", optional = true }
humantime = { version = "2.1.0", optional = true }
log = { version = "0.4.17", optional = true }

[profile.release]
strip = true
rustflags = ["-C", "target-feature=+crt-static"]

[package.metadata.winres]
OriginalFilename = "giuroll.dll"

[build-dependencies]
winres = "0.1"



================================================
File: giuroll.ini
================================================
[Keyboard]
; uses a different format than the one used by sokuroll,
; full list of accessible codes accessible at https://handmade.network/forums/articles/t/2823-keyboard_inputs_-_scancodes%252C_raw_input%252C_text_input%252C_key_names
;
; common values
;
; numpad minus: 0x4A
; numpad plus : 0x4E
;
; U: 0x16
; I: 0x17
;
; 9 = 0x0A,
; 0 = 0x0B,
;
; -/_ = 0x0C
; =/+ = 0x0D
;

decrease_delay_key=0x0A
increase_delay_key=0x0B
toggle_network_stats=0x09

[Netplay]
default_delay=2

enable_network_stats_by_default=no

enable_auto_delay=yes
; target rollback for auto delay, 
; higher value = less delay
; recommend values between 1 and 3 
auto_delay_rollback=2

; helps with frame one freezes caused by high packet loss, may cause an issue with a particular mod
frame_one_freeze_mitigation=yes


[FramerateFix]
; spinning is used to "sleep" for sub millisecond intervals. This immensely increases CPU usage, but should improve frame pacing. Especially noticable on 60 hz screens.
; values above 1500 are unlikely to bring much of a positive effect. 0 disables spinning entirely
spin_amount=1500
; sets framerate to 62 FPS, used to play with Chinese players
enable_f62=no

[Misc]
; $ will be replaced with current giuroll version. Unicode is supported as long as your locale the characters
game_title="Touhou Hisoutensoku + $"

; Temporary solution to set default character mod as soku2, untill soku2 team can set it themselves
soku2_compatibility_mode=yes

; Enable println! which prints logs. `println!` sometimes crashes the game because of the error on printing.
enable_println=no



================================================
File: LICENSE
================================================
MIT License

Copyright (c) 2023 Giufin

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


================================================
File: ilhookmod/Cargo.toml
================================================
# THIS FILE IS AUTOMATICALLY GENERATED BY CARGO
#
# When uploading crates to the registry Cargo will automatically
# "normalize" Cargo.toml files for maximal compatibility
# with all versions of Cargo and also rewrite `path` dependencies
# to registry (e.g., crates.io) dependencies.
#
# If you are reading this file be aware that the original Cargo.toml
# will likely look very different (and much more reasonable).
# See Cargo.toml.orig for the original contents.

[package]
edition = "2021"
name = "ilhook"
version = "2.0.0"
authors = ["regomne <fallingsunz@gmail.com>"]
description = "A library that provides methods to inline hook binary codes in x86 and x86_64 architecture"
readme = "README.md"
keywords = [
    "hook",
    "assemble",
    "disassemble",
]
categories = ["hardware-support"]
license = "MIT"
repository = "https://github.com/regomne/ilhook-rs"

[profile.release]
opt-level = 3
lto = true
codegen-units = 1


[target."cfg(windows)".dependencies.windows-sys]
version = "0.42.0"
features = [
    "Win32_Foundation",
    "Win32_System_Memory",
]



================================================
File: ilhookmod/LICENSE
================================================
MIT License

Copyright (c) 2023 regomne

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


================================================
File: ilhookmod/src/lib.rs
================================================
/*!
This crate provides methods to inline hook binary codes of `x86` and `x64` instruction sets.

HOOK is a mechanism that intercepts function calls and handles them by user-defined code.

# Installation

This crate works with Cargo and is on
[crates.io](https://crates.io/crates/ilhook). Add it to your `Cargo.toml`
like so:

```toml
[dependencies]
ilhook = "2"
```

# Hook Types

Ilhook supports 4 types of hooking.

## Jmp-back hook

This type is used when you want to get some information, or modify some values (parameters, stack vars, heap vars, etc.) at the specified timing.

Assume we have a C++ function:

```cpp
void check_serial_number(std::string& sn){
    uint32_t machine_hash = get_machine_hash();
    uint32_t sn_hash = calc_hash(sn);

    // we want to modify the result of this comparison.
    if (sn_hash == machine_hash) {
        // success
    }
    // fail
}
```

And it compiles to the asm code:

```asm
0x401054 call get_machine_hash   ;get_machine_hash()
0x401059 mov ebx, eax

; ...

0x401070 lea eax, sn
0x401076 push eax
0x401077 call calc_hash          ;calc_hash(sn)
0x40107C add esp, 4
0x40107F cmp eax, ebx            ;we want to modify the eax here!
0x401081 jnz _check_fail

; check_success
```

Now let's start:

```rust
# #[cfg(target_arch = "x86")]
use ilhook::x86::{Hooker, HookType, Registers, CallbackOption, HookFlags};

# #[cfg(target_arch = "x86")]
unsafe extern "C" fn on_check_sn(reg:*mut Registers, _:usize){
    println!("machine_hash: {}, sn_hash: {}", (*reg).ebx, (*reg).eax);
    (*reg).eax = (*reg).ebx; //we modify the sn_hash!
}

# #[cfg(target_arch = "x86")]
let hooker=Hooker::new(0x40107F, HookType::JmpBack(on_check_sn), CallbackOption::None, 0, HookFlags::empty());
//hooker.hook().unwrap(); //commented as hooking is not supported in doc tests
```

Then `check_serial_number` will always go to the successful path.

## Function hook

This type is used when you want to replace a function with your customized
function. Note that you should only hook at the beginning of a function.

Assume we have a function:

```rust
fn foo(x: u64) -> u64 {
    x * x
}

assert_eq!(foo(5), 25);
```

And you want to let it return `x*x+3`, which means foo(5)==28.

Now let's hook:

```rust
# #[cfg(target_arch = "x86_64")]
use ilhook::x64::{Hooker, HookType, Registers, CallbackOption, HookFlags};
# #[cfg(target_arch = "x86_64")]
# fn foo(x: u64) -> u64 {
#     x * x
# }
# #[cfg(target_arch = "x86_64")]
unsafe extern "win64" fn new_foo(reg:*mut Registers, _:usize, _:usize)->usize{
    let x = (&*reg).rdi as usize;
    x*x+3
}

# #[cfg(target_arch = "x86_64")]
let hooker=Hooker::new(foo as usize, HookType::Retn(new_foo), CallbackOption::None, 0, HookFlags::empty());
unsafe{hooker.hook().unwrap()};
//assert_eq!(foo(5), 28); //commented as hooking is not supported in doc tests
```

## Jmp-addr hook

This type is used when you want to change the original run path to any other you wanted.

The first element of the enum `HookType::JmpToAddr` indicates where you want the EIP jump
to after the callback routine returns.

## Jmp-ret hook

This type is used when you want to change the original run path to any other you wanted, and
the destination address may change by the input arguments.

The EIP will jump to the value the callback routine returns.

# Notes

This crate is not thread-safe if you don't specify `HookFlags::NOT_MODIFY_MEMORY_PROTECT`. Of course,
you need to modify memory protection of the destination address by yourself if you specify that.

As rust's test run parrallelly, it may crash if not specify `--test-threads=1`.

*/

#![warn(missing_docs)]

/// The x86 hooker
pub mod x86;




================================================
File: ilhookmod/src/x86.rs
================================================
use std::io::{Cursor, Seek, SeekFrom, Write};
use std::slice;

#[cfg(windows)]
use core::ffi::c_void;

#[cfg(windows)]
use windows_sys::Win32::System::Memory::VirtualProtect;

const JMP_INST_SIZE: usize = 5;

/// This is the routine used in a `jmp-back hook`, which means the EIP will jump back to the
/// original position after the routine has finished running.
///
/// # Arguments
///
/// * `regs` - The registers
/// * `user_data` - User data that was previously passed to [`Hooker::new`].
pub type JmpBackRoutine = unsafe extern "cdecl" fn(regs: *mut Registers, user_data: usize);

/// This is the routine used in a `function hook`, which means the routine will replace the
/// original function and the EIP will `retn` directly instead of jumping back.
/// Note that the address being hooked must be the start of a function.
///
/// # Parameters
///
/// * `regs` - The registers.
/// * `ori_func_ptr` - The original function pointer. Call this after converting it to the original function type.
/// * `user_data` - User data that was previously passed to [`Hooker::new`].
///
/// # Return value
///
/// Returns the new return value of the replaced function.
pub type RetnRoutine =
    unsafe extern "cdecl" fn(regs: *mut Registers, ori_func_ptr: usize, user_data: usize) -> usize;

/// This is the routine used in a `jmp-addr hook`, which means the EIP will jump to the specified
/// address after the routine has finished running.
///
/// # Parameters
///
/// * `regs` - The registers.
/// * `ori_func_ptr` - The original function pointer. Call this after converting it to the original function type.
/// * `user_data` - User data that was previously passed to [`Hooker::new`].
pub type JmpToAddrRoutine =
    unsafe extern "cdecl" fn(regs: *mut Registers, ori_func_ptr: usize, src_addr: usize);

/// This is the routine used in a `jmp-ret hook`, which means the EIP will jump to the return
/// value of the routine.
///
/// # Parameters
///
/// * `regs` - The registers.
/// * `ori_func_ptr` - The original function pointer. Call this after converting it to the original function type.
/// * `user_data` - User data that was previously passed to [`Hooker::new`].
///
/// # Return value
///
/// Returns the address you want to jump to.
pub type JmpToRetRoutine =
    unsafe extern "cdecl" fn(regs: *mut Registers, ori_func_ptr: usize, src_addr: usize) -> usize;

/// The hooking type.
pub enum HookType {
    /// Used in a jmp-back hook
    JmpBack(JmpBackRoutine),

    /// Used in a function hook. The first element is the mnemonic of the `retn`
    /// instruction.
    Retn(usize, RetnRoutine),

    /// Used in a jmp-addr hook. The first element is the destination address
    JmpToAddr(usize, u8, JmpToAddrRoutine),

    /// Used in a jmp-ret hook.
    JmpToRet(JmpToRetRoutine),
}

/// The common registers.
#[repr(C)]
#[derive(Debug)]
pub struct Registers {
    /// The flags register.
    pub eflags: u32,
    /// The edi register.
    pub edi: u32,
    /// The esi register.
    pub esi: u32,
    /// The ebp register.
    pub ebp: u32,
    /// The esp register.
    pub esp: u32,
    /// The ebx register.
    pub ebx: u32,
    /// The edx register.
    pub edx: u32,
    /// The ecx register.
    pub ecx: u32,
    /// The eax register.
    pub eax: u32,
}

impl Registers {
    /// Get the value by the index from register `esp`.
    ///
    /// # Parameters
    ///
    /// * cnt - The index of the arguments.
    ///
    /// # Safety
    ///
    /// Process may crash if register `esp` does not point to a valid stack.
    #[must_use]
    pub unsafe fn get_arg(&self, cnt: usize) -> u32 {
        *((self.esp as usize + cnt * 4) as *mut u32)
    }
}

/// The trait which is called before and after the modifying of the `jmp` instruction.
/// Usually is used to suspend and resume all other threads, to avoid instruction colliding.
pub trait ThreadCallback {
    /// the callback before modifying `jmp` instruction, should return true if success.
    fn pre(&self) -> bool;
    /// the callback after modifying `jmp` instruction
    fn post(&self);
}

/// The entry struct in ilhook.
/// Please read the main doc to view usage.
pub struct Hooker {
    addr: usize,
    hook_type: HookType,

    user_data: usize,
}

/// The hook result returned by `Hooker::hook`.
pub struct HookPoint {
    addr: usize,
    stub: Box<[u8; 100]>,
    stub_prot: u32,
}

#[cfg(not(target_arch = "x86"))]
fn env_lock() {
    panic!("This crate should only be used in arch x86_32!")
}
#[cfg(target_arch = "x86")]
fn env_lock() {}

impl Hooker {
    /// Create a new Hooker.
    ///
    /// # Parameters
    ///
    /// * `addr` - The being-hooked address.
    /// * `hook_type` - The hook type and callback routine.
    /// * `thread_cb` - The callbacks before and after hooking.
    /// * `flags` - Hook flags
    #[must_use]
    pub fn new(addr: usize, hook_type: HookType, user_data: usize) -> Self {
        env_lock();
        Self {
            addr,
            hook_type,

            user_data,
        }
    }

    /// Consumes self and execute hooking. Return the `HookPoint`.
    ///
    /// # Safety
    ///
    /// Process may crash (instead of panic!) if:
    ///
    /// 1. addr is not an accessible memory address, or is not long enough.
    /// 2. addr points to an incorrect position. (At the middle of an instruction, or where after it other instructions may jump to)
    /// 3. Wrong Retn-val if `hook_type` is `HookType::Retn`. i.e. A `cdecl` function with non-zero retn-val, or a `stdcall` function with wrong retn-val.
    /// 4. Set `NOT_MODIFY_MEMORY_PROTECT` where it should not be set.
    /// 5. hook or unhook from 2 or more threads at the same time without `HookFlags::NOT_MODIFY_MEMORY_PROTECT`. Because of memory protection colliding.
    /// 6. Other unpredictable errors.
    #[must_use]
    pub unsafe fn hook(self, len: usize) -> HookPoint {
        let origin = get_moving_insts(self.addr, len);
        let ol = origin.len() as u8;
        let stub = generate_stub(&self, origin.clone(), ol, self.user_data);
        let stub_prot = modify_mem_protect(stub.as_ptr() as usize, stub.len());

        let old_prot = modify_mem_protect(self.addr, JMP_INST_SIZE);
        modify_jmp_with_thread_cb(&self, stub.as_ptr() as usize);
        recover_mem_protect(self.addr, JMP_INST_SIZE, old_prot);

        HookPoint {
            addr: self.addr,
            stub,
            stub_prot,
        }
    }
}

impl HookPoint {
    /// Consume self and unhook the address.
    pub unsafe fn unhook(self) {
        self.unhook_by_ref()
    }

    fn unhook_by_ref(&self) {
        let old_prot = modify_mem_protect(self.addr, JMP_INST_SIZE);

        recover_mem_protect(self.addr, JMP_INST_SIZE, old_prot);

        recover_mem_protect(self.stub.as_ptr() as usize, self.stub.len(), self.stub_prot);
    }
}

// When the HookPoint drops, it should unhook automatically.
impl Drop for HookPoint {
    fn drop(&mut self) {
        self.unhook_by_ref();
    }
}

fn get_moving_insts(addr: usize, len: usize) -> Vec<u8> {
    let code_slice = unsafe { slice::from_raw_parts(addr as *const u8, len) };
    //let mut decoder = Decoder::new(32, code_slice, DecoderOptions::NONE);
    //decoder.set_ip(addr as u64);

    //code_slice[0..decoder.position()].into()
    code_slice.into()
}

#[cfg(windows)]
pub fn modify_mem_protect(addr: usize, len: usize) -> u32 {
    let mut old_prot: u32 = 0;
    let old_prot_ptr = std::ptr::addr_of_mut!(old_prot);
    // PAGE_EXECUTE_READWRITE = 0x40
    let ret = unsafe { VirtualProtect(addr as *const c_void, len, 0x40, old_prot_ptr) };

    old_prot
}

#[cfg(unix)]
fn modify_mem_protect(addr: usize, len: usize) -> Result<u32, HookError> {
    let page_size = unsafe { sysconf(30) }; //_SC_PAGESIZE == 30
    if len > page_size.try_into().unwrap() {
        Err(HookError::InvalidParameter)
    } else {
        //(PROT_READ | PROT_WRITE | PROT_EXEC) == 7
        let ret = unsafe {
            mprotect(
                (addr & !(page_size as usize - 1)) as *mut c_void,
                page_size as usize,
                7,
            )
        };
        if ret != 0 {
            let err = unsafe { *(__errno_location()) };
            Err(HookError::MemoryProtect(err as u32))
        } else {
            // it's too complex to get the original memory protection
            Ok(7)
        }
    }
}

#[cfg(windows)]
pub fn recover_mem_protect(addr: usize, len: usize, old: u32) {
    let mut old_prot: u32 = 0;
    let old_prot_ptr = std::ptr::addr_of_mut!(old_prot);
    unsafe { VirtualProtect(addr as *const c_void, len, old, old_prot_ptr) };
}

#[cfg(unix)]
fn recover_mem_protect(addr: usize, _: usize, old: u32) {
    let page_size = unsafe { sysconf(30) }; //_SC_PAGESIZE == 30
    unsafe {
        mprotect(
            (addr & !(page_size as usize - 1)) as *mut c_void,
            page_size as usize,
            old as i32,
        )
    };
}

fn write_relative_off<T: Write + Seek>(buf: &mut T, base_addr: u32, dst_addr: u32) {
    let dst_addr = dst_addr as i32;
    let cur_pos = buf.stream_position().unwrap() as i32;
    let call_off = dst_addr - (base_addr as i32 + cur_pos + 4);
    buf.write(&call_off.to_le_bytes()).unwrap();
}

//fn move_code_to_addr(ori_insts: &Vec<u8>, dest_addr: u32) -> Vec<u8> {
//    let block = InstructionBlock::new(ori_insts, u64::from(dest_addr));
//    let encoded = BlockEncoder::encode(32, block, BlockEncoderOptions::NONE)
//        .map_err(|_| HookError::MoveCode)?;
//    Ok(encoded.code_buffer)
//}

fn write_ori_func_addr<T: Write + Seek>(buf: &mut T, ori_func_addr_off: u32, ori_func_off: u32) {
    let pos = buf.stream_position().unwrap();
    buf.seek(SeekFrom::Start(u64::from(ori_func_addr_off)))
        .unwrap();
    buf.write(&ori_func_off.to_le_bytes()).unwrap();
    buf.seek(SeekFrom::Start(pos)).unwrap();
}

fn generate_jmp_back_stub<T: Write + Seek>(
    buf: &mut T,
    stub_base_addr: u32,
    moving_code: &Vec<u8>,
    ori_addr: u32,
    cb: JmpBackRoutine,
    ori_len: u8,
    user_data: usize,
) {
    // push user_data
    buf.write(&[0x68]).unwrap();
    buf.write(&user_data.to_le_bytes()).unwrap();

    // push ebp (Registers)
    // call XXXX (dest addr)
    buf.write(&[0x55, 0xe8]).unwrap();
    write_relative_off(buf, stub_base_addr, cb as u32);

    // add esp, 0x8
    buf.write(&[0x83, 0xc4, 0x08]).unwrap();
    // popfd
    // popad
    buf.write(&[0x9d, 0x61]).unwrap();

    buf.write(&moving_code).unwrap();
    // jmp back
    buf.write(&[0xe9]).unwrap();

    write_relative_off(buf, stub_base_addr, ori_addr + u32::from(ori_len))
}

fn generate_retn_stub<T: Write + Seek>(
    buf: &mut T,
    stub_base_addr: u32,
    moving_code: &Vec<u8>,
    ori_addr: u32,
    retn_val: u16,
    cb: RetnRoutine,
    ori_len: u8,
    user_data: usize,
) {
    // push user_data
    buf.write(&[0x68]).unwrap();
    buf.write(&user_data.to_le_bytes()).unwrap();

    // push XXXX (original function addr)
    // push ebp (Registers)
    // call XXXX (dest addr)
    let ori_func_addr_off = buf.stream_position().unwrap() + 1;
    buf.write(&[0x68, 0, 0, 0, 0, 0x55, 0xe8]).unwrap();
    write_relative_off(buf, stub_base_addr, cb as u32);

    // add esp, 0xc
    buf.write(&[0x83, 0xc4, 0x0c]).unwrap();
    // mov [esp+20h], eax
    buf.write(&[0x89, 0x44, 0x24, 0x20]).unwrap();
    // popfd
    // popad
    buf.write(&[0x9d, 0x61]).unwrap();
    if retn_val == 0 {
        // retn
        buf.write(&[0xc3]).unwrap();
    } else {
        // retn XX
        buf.write(&[0xc2]).unwrap();
        buf.write(&retn_val.to_le_bytes()).unwrap();
    }
    let ori_func_off = buf.stream_position().unwrap() as u32;
    write_ori_func_addr(buf, ori_func_addr_off as u32, stub_base_addr + ori_func_off);

    buf.write(&moving_code).unwrap();

    // jmp ori_addr
    buf.write(&[0xe9]).unwrap();
    write_relative_off(buf, stub_base_addr, ori_addr + u32::from(ori_len))
}

fn generate_jmp_addr_stub<T: Write + Seek>(
    buf: &mut T,
    stub_base_addr: u32,
    moving_code: &Vec<u8>,
    ori_addr: u32,
    dest_addr: u32,
    cb: JmpToAddrRoutine,
    ori_len: u8,
    user_data: usize,
    popstack: u8,
) {
    // push user_data
    buf.write(&[0x68]).unwrap();
    buf.write(&user_data.to_le_bytes()).unwrap();

    // push XXXX (original function addr)
    // push ebp (Registers)
    // call XXXX (dest addr)
    let ori_func_addr_off = buf.stream_position().unwrap() + 1;
    buf.write(&[0x68, 0, 0, 0, 0, 0x55, 0xe8]).unwrap();
    write_relative_off(buf, stub_base_addr, cb as u32);

    // add esp, 0xc
    buf.write(&[0x83, 0xc4, 0x0c]).unwrap();
    // popfd
    // popad
    buf.write(&[0x9d, 0x61]).unwrap();

    //pop stack
    buf.write(&[0x83, 0xC4, popstack]).unwrap();
    // jmp back
    buf.write(&[0xe9]).unwrap();
    write_relative_off(
        buf,
        stub_base_addr,
        dest_addr, /* + u32::from(ori_len) */
    );

    let ori_func_off = buf.stream_position().unwrap() as u32;
    write_ori_func_addr(buf, ori_func_addr_off as u32, stub_base_addr + ori_func_off);

    buf.write(&moving_code).unwrap();

    // jmp ori_addr
    buf.write(&[0xe9]).unwrap();
    write_relative_off(buf, stub_base_addr, ori_addr + u32::from(ori_len))
}

fn generate_jmp_ret_stub<T: Write + Seek>(
    buf: &mut T,
    stub_base_addr: u32,
    moving_code: &Vec<u8>,
    ori_addr: u32,
    cb: JmpToRetRoutine,
    ori_len: u8,
    user_data: usize,
) {
    // push user_data
    buf.write(&[0x68]).unwrap();
    buf.write(&user_data.to_le_bytes()).unwrap();

    // push XXXX (original function addr)
    // push ebp (Registers)
    // call XXXX (dest addr)
    let ori_func_addr_off = buf.stream_position().unwrap() + 1;
    buf.write(&[0x68, 0, 0, 0, 0, 0x55, 0xe8]).unwrap();
    write_relative_off(buf, stub_base_addr, cb as u32);

    // add esp, 0xc
    buf.write(&[0x83, 0xc4, 0x0c]).unwrap();
    // mov [esp-4], eax
    buf.write(&[0x89, 0x44, 0x24, 0xfc]).unwrap();
    // popfd
    // popad
    buf.write(&[0x9d, 0x61]).unwrap();
    // jmp dword ptr [esp-0x28]
    buf.write(&[0xff, 0x64, 0x24, 0xd8]).unwrap();

    let ori_func_off = buf.stream_position().unwrap() as u32;
    write_ori_func_addr(buf, ori_func_addr_off as u32, stub_base_addr + ori_func_off);

    buf.write(&moving_code).unwrap();

    // jmp ori_addr
    buf.write(&[0xe9]).unwrap();
    write_relative_off(buf, stub_base_addr, ori_addr + u32::from(ori_len))
}

fn generate_stub(
    hooker: &Hooker,
    moving_code: Vec<u8>,
    ori_len: u8,
    user_data: usize,
) -> Box<[u8; 100]> {
    let mut raw_buffer = Box::new([0u8; 100]);
    let stub_addr = raw_buffer.as_ptr() as u32;
    let mut buf = Cursor::new(&mut raw_buffer[..]);

    // pushad
    // pushfd
    // mov ebp, esp
    buf.write(&[0x60, 0x9c, 0x8b, 0xec]).unwrap();

    match hooker.hook_type {
        HookType::JmpBack(cb) => generate_jmp_back_stub(
            &mut buf,
            stub_addr,
            &moving_code,
            hooker.addr as u32,
            cb,
            ori_len,
            user_data,
        ),
        HookType::Retn(val, cb) => generate_retn_stub(
            &mut buf,
            stub_addr,
            &moving_code,
            hooker.addr as u32,
            val as u16,
            cb,
            ori_len,
            user_data,
        ),
        HookType::JmpToAddr(dest, popstack, cb) => generate_jmp_addr_stub(
            &mut buf,
            stub_addr,
            &moving_code,
            hooker.addr as u32,
            dest as u32,
            cb,
            ori_len,
            user_data,
            popstack,
        ),
        HookType::JmpToRet(cb) => generate_jmp_ret_stub(
            &mut buf,
            stub_addr,
            &moving_code,
            hooker.addr as u32,
            cb,
            ori_len,
            user_data,
        ),
    };

    raw_buffer
}

fn modify_jmp(dest_addr: usize, stub_addr: usize) {
    let buf = unsafe { slice::from_raw_parts_mut(dest_addr as *mut u8, JMP_INST_SIZE) };
    // jmp stub_addr
    buf[0] = 0xe9;
    let rel_off = stub_addr as i32 - (dest_addr as i32 + 5);
    buf[1..5].copy_from_slice(&rel_off.to_le_bytes());
}

fn modify_jmp_with_thread_cb(hook: &Hooker, stub_addr: usize) {
    modify_jmp(hook.addr, stub_addr)
}

#[cfg(target_arch = "x86")]
mod tests {
    #[allow(unused_imports)]
    use super::*;

    #[cfg(test)]
    #[inline(never)]
    fn foo(x: u32) -> u32 {
        println!("original foo, x:{}", x);
        x * x
    }
    #[cfg(test)]
    unsafe extern "cdecl" fn on_foo(
        reg: *mut Registers,
        old_func: usize,
        user_data: usize,
    ) -> usize {
        let old_func = std::mem::transmute::<usize, fn(u32) -> u32>(old_func);
        old_func((*reg).get_arg(1)) as usize + user_data
    }

    #[test]
    fn test_hook_function_cdecl() {
        assert_eq!(foo(5), 25);
        let hooker = Hooker::new(
            foo as usize,
            HookType::Retn(0, on_foo),
            CallbackOption::None,
            100,
            HookFlags::empty(),
        );
        let info = unsafe { hooker.hook().unwrap() };
        assert_eq!(foo(5), 125);
        unsafe { info.unhook().unwrap() };
        assert_eq!(foo(5), 25);
    }

    #[cfg(test)]
    #[inline(never)]
    extern "stdcall" fn foo2(x: u32) -> u32 {
        println!("original foo, x:{}", x);
        x * x
    }
    #[cfg(test)]
    unsafe extern "cdecl" fn on_foo2(
        reg: *mut Registers,
        old_func: usize,
        user_data: usize,
    ) -> usize {
        let old_func = std::mem::transmute::<usize, extern "stdcall" fn(u32) -> u32>(old_func);
        old_func((*reg).get_arg(1)) as usize + user_data
    }
    #[test]
    fn test_hook_function_stdcall() {
        assert_eq!(foo2(5), 25);
        let hooker = Hooker::new(
            foo2 as usize,
            HookType::Retn(4, on_foo2),
            CallbackOption::None,
            100,
            HookFlags::empty(),
        );
        let info = unsafe { hooker.hook().unwrap() };
        assert_eq!(foo2(5), 125);
        unsafe { info.unhook().unwrap() };
        assert_eq!(foo2(5), 25);
    }
}



================================================
File: injector/README.md
================================================
# Injector for the giuroll DLL

## Compilation
compile with `cargo` using one of the `_-686-pc-windows-msvc` toolchains.

## Usage
- Start th123.exe 
- Run the injector  

if sucessfull, you should see a message.  
If the injector closes abruptly, contact me about it.


================================================
File: injector/Cargo.toml
================================================
cargo-features = ["profile-rustflags"]

[package]
name = "injector"
authors = ["Giufin"]
version = "0.1.0"
edition = "2021"
license = "MIT"
# See more keys and their definitions at https://doc.rust-lang.org/cargo/reference/manifest.html

[dependencies]
dll-syringe = "0.15.2"

[profile.release]
strip = true
lto = true
opt-level = "s"
codegen-units = 1
panic = "abort"
rustflags = ["-C", "target-feature=+crt-static"]



================================================
File: injector/LICENSE
================================================
Copyright (c) 2023 Giufin

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


================================================
File: injector/.gitignore
================================================
/target
/Cargo.lock



================================================
File: injector/src/main.rs
================================================
use std::{ffi::OsStr, path::PathBuf, time::Duration};

use dll_syringe::{process::OwnedProcess, Syringe};

fn main() {
    unsafe {
        /*
        let tl = tasklist::Tasklist::new();

        for i in tl {
            if i.get_pname() == "th123.exe" {
                let m = i.get_pid();
                let proc = djin::open_process(m).unwrap();
                let dll = "giuroll.dll";
                djin::inject_dll(proc, dll, "exeinit".as_bytes()).unwrap();
                std::thread::sleep(std::time::Duration::from_secs(5));
            }
        }
        */

        let target_process = match OwnedProcess::find_first_by_name("th123.exe") {
            Some(x) => x,
            None => {
                println!("th123.exe process not found, make sure soku is running");
                std::thread::sleep(Duration::from_secs(5));
                panic!()
            }
        };
        let syringe = Syringe::for_process(target_process);
        let injected_payload = syringe.inject("giuroll.dll").unwrap();

        match syringe
            .get_raw_procedure::<unsafe extern "C" fn() -> bool>(
                injected_payload,
                "better_exe_init",
            )
            .ok()
            .flatten()
            .map(|f1| {
                (
                    f1,
                    syringe
                        .get_raw_procedure::<unsafe extern "C" fn(u8)>(
                            injected_payload,
                            "better_exe_init_push_path",
                        )
                        .unwrap()
                        .unwrap(),
                )
            })
            .and_then(|function| {
                std::env::current_dir()
                    .ok()
                    .map(move |path| (function, path))
            }) {
            Some(((f1, f2), current_path)) => {
                let slice = current_path.as_os_str().as_encoded_bytes();
                for a in slice {
                    f2.call(*a).unwrap();
                }
                if f1.call().unwrap() {
                    println!("injection successfull")
                } else {
                    println!("injection failed, giuroll.ini not found")
                }
            }
            None => syringe
                .get_raw_procedure::<unsafe extern "C" fn()>(injected_payload, "exeinit")
                .unwrap()
                .unwrap()
                .call()
                .unwrap(),
        };
    }
    std::thread::sleep(Duration::from_secs(5));
}



================================================
File: mininip/README.md
================================================
# MinIniP

## What is MinIniP ?
### Presentation
**MinIniP** stands for Minimalist INI Parser. It is a parser written in Rust to
store datas in an easy and safe way. Currently, it is not intended to be
extremly fast and there is not any benchmark of it. You can use it like you want
in any project. Every change of one of the provided files must be published
under the MPL-2.0.

### Why MinIniP ?
Honnestly, there is not any particular reason to chose MinIniP. I just wrote it
to play with Rust wich I learn a few months ago. I will use it in my personnal
projects so it will be actively maintained for a long time.

### You convinced me ! How to use it ?

Just add

```toml
mininip="1.2"
```

to your `Cargo.toml` in the `dependencies` section and you are right ! You can
also download it at 
[the official repository](https://github.com/BorisDRYKONINGEN/mininip).

## What is a valid INI file ?
### A lack of standardisation
Since there is not any standard INI specification, each implementor writes its
own. Here is mine. You can contribute to the project by extending this
specification if you think something is missing. The only rule to follow is to
not break backward compatibility, except in one case: adding a new INI type may
break some use cases of the `Raw` type by moving a declaration of variable to
the new type.

For instance, you may create a `Set` INI type which is like the sets in maths,
and is enclosed by curly brackets `{}`. In this way, parsing this line

```ini
an INI key = { Hello, world, ! }
```

will no longer produce a `Value::Raw` value when parsing it but a `Value::Set`
instead.

### The specification followed by MinIniP
#### Identifiers
An identifier refers to either
* A section name
* A key name

An identifier must start with one of `.`, `$`, `:` or `a-zA-Z`. Since the second
character, all of `_`, `~`, `-`, `.`, `:`, `$`, `a-zA-Z` and `0-9` are allowed.
In the API, an `Identifier` refers to a combination of a section name and a key
name, so keep in mind *it is not just a key name* !

The specification above might be outdated, so refer to the generated
documentation (`Identifier::is_valid`) to be aware of what is a valid INI
identifier.

#### Values
##### Declaring a value
A value can be anything following the first `=` sign in an expression. It must
be assigned to a key and not a section. In this code

```ini
key=value
```

`key` must be a valid identifier and `value` is defined as the value.

##### Types
A value can be either

* `Raw` a valid value which does not match with any of the types below
* `Str` a valid value inside two quotes `'` or `"`
* `Int` a 64-bits-sized integer
* `Float` a 64-bits-sized floating-point number
* `Bool` a boolean (either `true` (`on`, `enabled`, `y` or `yes`) or `false` (`off`, `disabled`, `n` or `no`))

The highest priority is for the type `Str`. Since quotes are forbidden in all
the other use cases, a quoted value can only be a `Str`. Then, comes the `Bool`
type which only allows a few values (see above). Then, comes `Int` and in case
of failure while interpretting it as an integer, `Float`. If none of these types
match with the given value, the value is `Raw` which is the value as written in
the file (after unescaping, defined below).

##### Escape sequences
In an INI file, all the possible values are **not accepted**. For instance, you 
cannot store an emoji (like ☺ or ♥) or any other non-ASCII character *as is*
in a file. It is even true for characters which may be part of the INI syntax
like the semicolon `;`, the colon `:`, the equal sign `=`... However, you have
not to deal with these characters since MinIniP does it for you. The characters
are *escaped*. Here is an exhaustive list of the recognized escapes sequences.

| Non-escaped       | Escaped |
| :---------------- | :-----: |
| `\`               | `\\`    |
| `'`               | `\'`    |
| `"`               | `\"`    |
| null character    | `\0`    |
| bell / alert      | `\a`    |
| backspace         | `\b`    |
| tab character     | `\t`    |
| carriage return   | `\r`    |
| line feed         | `\n`    |
| `;`               | `\;`    |
| `#`               | `\#`    |
| `=`               | `\=`    |
| `:`               | `\:`    |
| unicode character | `\xxxxxx` with `xxxxxx` corresponding to its hexadecimal code (six digits) |

Please note that escapes are **not available** for identifiers.

#### Sections
A section refers to what can be called in Rust a module, or a namespace in C++.
In a few words, it is a named or anonymous set of keys. A section identifier
must be a valid identifier or nothing at all. A section is declared by putting
square brackets `[]` around its identifier on a line.

```ini
key_1 = value_1
key_2 = value_2

[section 1]
key_1 = value_3
key_2 = value_4

[section 2]
key_1 = value_5
key_2 = value_6
```

In this code, every occurences of `key_1` and `key_2` are different keys because
they are not in the same section. The first ones are in the global / anonymous
section, the two following ones in a `section 1`-named section, and the two last
ones in a `section 2`-named section.

In this API, section names are the first value stored in an `Identifier`. It is
a `Option<String>` since a section may be anonymous (the keys declared before
the first section are in the anonymous section corresponding to `None`). All the
named sections are represented as `Some(name)`. The second value is the key,
which is a `String` that must be a valid identifier.



================================================
File: mininip/bad.ini
================================================
This file is not an INI file



================================================
File: mininip/Cargo.toml
================================================
[package]
name = "mininip"
version = "1.3.1"
authors = ["Boris DRYKONINGEN <boris.d@orange.fr>"]
edition = "2018"
license = "MPL-2.0"
description = "A minimalist ini file parser (MinIniP stands for Minimalist Ini Parser). It is written in Rust but I will export its API to the C programming language in order to make various bindings"
repository = "https://github.com/BorisDRYKONINGEN/mininip"

[lib]
crate-type = ["lib"]

[dependencies]



================================================
File: mininip/good.ini
================================================
author = "Boris DRYKONINGEN"
version_major = 0

[numbers]
one = 1
two = 2
three = 3

[symbols]
smiley = \x00263a
semicolon = \;

[valid since 1.2.0]
contains spaces = on
$starts-with-$ = on
contains: = on



================================================
File: mininip/.gitignore
================================================
target
Cargo.lock
**/*.rs.bk



================================================
File: mininip/src/lib.rs
================================================
//! An minimalist ini file parser (MinIniP stands for Minimalist Ini Parser). It is written in Rust but I would export its API to the C programming language in order to make various bindings

pub mod datas;
pub mod dump;
pub mod parse;
pub mod errors;

#[cfg(test)]
mod tests;



================================================
File: mininip/src/tests.rs
================================================
use crate::{parse, dump, errors};
use parse::parse_file;
use errors::ParseFileError;
use crate::datas::{Identifier, Value};
use std::collections::HashMap;
use dump::dump_into_file;
use std::fs::{self, File};
use std::io::Read;

#[test]
fn parse_reverses_dump() {
    let message = "Hello world ☺. 1+1=2; 2+2=4 \\0/";

    assert_eq!(parse::parse_str(&dump::dump_str(message)).expect("`dump_str` must return a well escaped string"), message);
}

#[test]
fn parse_good_file() {
    let data = parse_file("good.ini").unwrap();

    let author = Identifier::new(None, String::from("author"));
    let version_major = Identifier::new(None, String::from("version_major"));

    let numbers = Some(String::from("numbers"));
    let one = Identifier::new(numbers.clone(), String::from("one"));
    let two = Identifier::new(numbers.clone(), String::from("two"));
    let three = Identifier::new(numbers, String::from("three"));

    let symbols = Some(String::from("symbols"));
    let smiley = Identifier::new(symbols.clone(), String::from("smiley"));
    let semicolon = Identifier::new(symbols, String::from("semicolon"));

    let valid_since_1_2_0 = Some(String::from("valid since 1.2.0"));
    let contains_spaces = Identifier::new(valid_since_1_2_0.clone(), String::from("contains spaces"));
    let starts_with_dollar = Identifier::new(valid_since_1_2_0.clone(), String::from("$starts-with-$"));
    let contains_colon = Identifier::new(valid_since_1_2_0, String::from("contains:"));

    println!("{:?}", data);

    assert_eq!(data[&author], Value::Str(String::from("Boris DRYKONINGEN")));
    assert_eq!(data[&version_major], Value::Int(0));

    assert_eq!(data[&one], Value::Int(1));
    assert_eq!(data[&two], Value::Int(2));
    assert_eq!(data[&three], Value::Int(3));

    assert_eq!(data[&smiley], Value::Raw(String::from("\u{263a}")));
    assert_eq!(data[&semicolon], Value::Raw(String::from(";")));

    assert_eq!(data[&contains_spaces], Value::Bool(true));
    assert_eq!(data[&starts_with_dollar], Value::Bool(true));
    assert_eq!(data[&contains_colon], Value::Bool(true));
}

#[test]
fn parse_bad_file() {
    let err = parse_file("bad.ini");
    match err {
        Ok(_)                              => panic!("This file contains wrong code and shouldn't be allowed"),
        Err(ParseFileError::ParseError(_)) => {},
        Err(err)                           => panic!("Wrong error value returned: {:?}", err),
    }
}

#[test]
fn parse_non_existing_file() {
    let err = parse_file("This file shouldn't exist. If you see it, remove it now.ini");
    match err {
        Ok(_)                           => panic!("This file does not exist. If it exists, remove it"),
        Err(ParseFileError::IOError(_)) => {},
        Err(err)                        => panic!("Wrong error value returned: {:?}", err),
    }
}

#[test]
fn test_dump_into_file() {
    let mut data = HashMap::new();

    let insert = &mut |section, ident, val| {
        let ident = Identifier::new(section, String::from(ident));
        data.insert(ident, val);
    };

    let section = None;
    insert(section.clone(), "abc", Value::Int(123));
    insert(section.clone(), "def", Value::Int(456));
    insert(section,         "ghi", Value::Int(789));

    let section = Some(String::from("maths"));
    insert(section.clone(), "sum",      Value::Str(String::from("∑")));
    insert(section.clone(), "sqrt",     Value::Str(String::from("√")));
    insert(section,         "infinity", Value::Str(String::from("∞")));

    let path = "test dump.ini";
    dump_into_file(path, data).unwrap();

    let expected = "\
    abc=123\n\
    def=456\n\
    ghi=789\n\
    \n\
    [maths]\n\
    infinity='\\x00221e'\n\
    sqrt='\\x00221a'\n\
    sum='\\x002211'\n";

    let mut file = File::open(path)
        .expect("Created above");
    let mut content = String::with_capacity(expected.len());
    file.read_to_string(&mut content).unwrap();

    std::mem::drop(file);
    if let Err(err) = fs::remove_file(path) {
        eprintln!("Error while removing the file: {}", err);
    }

    assert_eq!(content, expected);
}



================================================
File: mininip/src/datas/mod.rs
================================================
//! The basic datas structures like [`Identifier`](datas/struct.Identifier.html "Identifier") and [`Value`](datas/enum.Value.html "Value")

use std::fmt::{self, Display, Formatter};
use crate::{parse, dump};
use crate::errors::{Error, error_kinds::*};

/// The value of a INI variable
/// 
/// The following types are available
/// - `Raw`: the raw content of the file, not formatted. The only computation is that the escaped characters are unescaped (see [parse_str](../parse/fn.parse_str.html "parse::parse_str") to learn more about escaped characters)
/// - `Str`: a quoted string written inside non-escaped quotes like that `"Hello world!"` or that `'Hello world!'`
/// - `Int`: a 64 bytes-sized integer
/// - `Float`: a 64 bytes-sized floating-point number
/// - `Bool`: a boolean (currently either `on` or `off`)
/// 
/// Each type is represented as an enum variant
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Raw(String),
    Str(String),
    Int(i64),
    Float(f64),
    Bool(bool),
}

impl Display for Value {
    fn fmt(&self, formatter: &mut Formatter) -> fmt::Result {
        match self {
            Value::Raw(string)   => string.fmt(formatter),
            Value::Str(string)   => string.fmt(formatter),
            Value::Int(number)   => number.fmt(formatter),
            Value::Float(number) => number.fmt(formatter),
            Value::Bool(true)    => "on".fmt(formatter),
            Value::Bool(false)   => "off".fmt(formatter),
        }
    }
}

impl Default for Value {
    fn default() -> Self {
        Value::Raw(String::new())
    }
}

impl Value {
    /// Builds a new [`Value`](enum.Value.html "datas::Value") from `content`, an INI-formatted string
    /// 
    /// # Return value
    /// `Ok(value)` with `value` as the new object
    /// 
    /// `Err(error)` when an error occurs while parsing `content` with `error` as the error code
    pub fn parse(content: &str) -> Result<Value, Error> {
        let effective = content.trim();

        if effective.starts_with("'") || effective.starts_with("\"") {
            let quote = &effective[..1];

            if !effective.ends_with(quote) {
                let err = ExpectedToken::new(String::from(content), content.len(), String::from(quote));
                Err(Error::from(err))
            } else {
                let parsed = parse::parse_str(&effective[1..effective.len() - 1])?;
                Ok(Value::Str(parsed))
            }
        }

        else if effective == "on" || effective == "enabled" || effective == "y" || effective == "yes" {
            Ok(Value::Bool(true))
        } else if effective == "off" || effective == "disabled" || effective == "n" || effective == "no" {
            Ok(Value::Bool(false))
        }

        else if let Ok(value) = effective.parse::<i64>() {
            Ok(Value::Int(value))
        }

        else if let Ok(value) = effective.parse::<f64>() {
            Ok(Value::Float(value))
        }

        else {
            Ok(Value::Raw(parse::parse_str(effective)?))
        }
    }

    /// Formats `self` to be dumped in an INI file
    /// 
    /// It means that `format!("{}={}", ident, value.dump())` with `ident` as a valid key and `value` a [`Value`](enum.Value.html "Value") can be properly registered and then, parsed as INI
    /// 
    /// # Return value
    /// A `String` containing the value of `self` once formatted
    /// 
    /// # See
    /// See [`dump_str`](fn.dump_str.html "datas::dump_str") for more informations about this format
    /// 
    /// # Note
    /// `self` is backed up in a way preserving its type
    /// 
    /// - `Raw` is backed up as is, once escaped
    /// - `Str` is backed up with two quotes `'` or `"` around its value once escaped
    /// - `Int` is backed up as is
    /// - `Float` is backed up as is
    /// - `Bool` is backed up as two different values: `true` and `false`
    /// 
    /// # Examples
    /// ```
    /// use mininip::datas::Value;
    /// 
    /// let val = Value::Str(String::from("tr\\x0000e8s_content\\=\\x00263a \\; the symbol of hapiness"));
    /// let dumped = val.dump();
    /// 
    /// assert_eq!(dumped, "'tr\\x0000e8s_content\\=\\x00263a \\; the symbol of hapiness'"); // Notice the quotes here
    /// ```
    pub fn dump(&self) -> String {
        match self {
            Value::Raw(string)   => format!("{}", dump::dump_str(&string)),
            Value::Str(string)   => format!("'{}'", dump::dump_str(&string)),
            Value::Int(number)   => format!("{}", number),
            Value::Float(number) => format!("{}", number),
            Value::Bool(true)    => String::from("on"),
            Value::Bool(false)   => String::from("off"),
        }
    }
}


/// The identifier of a variable, which is its identity. Of course, this type is `Hash` because it may be used as a key in a `HashMap`
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Identifier {
    section: Option<String>,
    name: String,
}

impl Identifier {
    /// Creates an identifier with a valid section name and a valid name
    /// 
    /// # Panics
    /// Panics if either `section` or `name` is an invalid identifier according to [`Identifier::is_valid`](struct.Identifier.html#method.is_valid "datas::Identifier::is_valid")
    pub fn new(section: Option<String>, name: String) -> Identifier {
        if let Some(section) = &section {
            assert!(Identifier::is_valid(section));
        }
        assert!(Identifier::is_valid(&name));

        Identifier {
            section,
            name,
        }
    }

    /// Returns `true` if the given string is a valid identifier and `false` otherwise
    /// 
    /// A valid identifier is defined as a string of latin alphanumeric characters and any of `_`, `~`, `-`, `.`, `:`, `$` and space starting with a latin alphabetic one or any of `.`, `$` or `:`. All of these characters must be ASCII
    /// 
    /// # Notes
    /// Since the INI file format is not really normalized, this definition may evolve in the future. In fact, I will avoid when possible to make a stronger rule, in order to keep backward compatibility
    /// 
    /// # Examples
    /// ```
    /// use mininip::datas::Identifier;
    /// 
    /// assert!(Identifier::is_valid("identifier"));
    /// assert!(Identifier::is_valid("digits1230"));
    /// assert!(Identifier::is_valid("UPPERCASE_AND_UNDERSCORES"));
    /// assert!(Identifier::is_valid("contains spaces inside"));
    /// assert!(!Identifier::is_valid("123_starts_with_a_digit"));
    /// assert!(!Identifier::is_valid("invalid_characters;!\\~"));
    /// assert!(!Identifier::is_valid("é_is_unicode"));
    /// assert!(!Identifier::is_valid(" starts_with_a_space"));
    /// ```
    pub fn is_valid(ident: &str) -> bool {
        let mut iter = ident.chars();
        match iter.next() {
            // An empty string is not allowed
            None    => return false,

            // The first character must be a letter, a point, a dollar sign or a colon
            Some(c) => if !c.is_ascii() || !c.is_alphabetic() && c != '.' && c != '$' && c != ':' {
                return false;
            },
        }

        for i in iter {
            // The following ones may be numeric characters, underscores, tildes, dashs or spaces
            if !i.is_ascii() || !i.is_alphanumeric()
                             && i != '_' && i != '~' 
                             && i != '-' && i != '.'
                             && i != ':' && i != '$'
                             && i != ' ' {
                return false;
            }
        }

        true
    }

    /// Returns the name of the variable as a reference
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Returns the section of the variable which may be a named section as `Some(name)` or the "global scope" wich is `None`
    pub fn section(&self) -> Option<&str> {
        match &self.section {
            Some(val) => Some(&val),
            None      => None,
        }
    }

    /// Change the name of the variable
    /// 
    /// # Panics
    /// Panics if `name` is invalid according to [`Identifier::is_valid`](struct.Identifier.html#method.is_valid "datas::Identifier::is_valid")
    pub fn change_name(&mut self, name: String) {
        assert!(Identifier::is_valid(&name));

        self.name = name;
    }

    /// Changes the section of the variable. `section` may be `Some(name)` with `name` as the name of the section of `None` for the "global scope"
    /// 
    /// # Panics
    /// Panics if `section` is invalid according to [`Identifier::is_valid`](struct.Identifier.html#method.is_valid "datas::Identifier::is_valid")
    pub fn change_section(&mut self, section: Option<String>) {
        if let Some(section) = &section {
            assert!(Identifier::is_valid(section));
        }

        self.section = section;
    }
}

impl Display for Identifier {
    fn fmt(&self, formatter: &mut Formatter) -> fmt::Result {
        if let Some(section) = &self.section {
            formatter.write_str(&section)?;
            formatter.write_str(".")?;
        }

        formatter.write_str(&self.name)
    }
}


pub mod tree;

#[cfg(test)]
mod tests;



================================================
File: mininip/src/datas/tests.rs
================================================
use crate::datas::*;
use crate::errors::Error;

#[test]
fn value_display() {
    let txt = "Hello world!";
    let val = Value::Raw(String::from(txt));

    assert_eq!(format!("{}", val), txt);
}

#[test]
fn value_dump() {
    let val = Value::Raw(String::from("tr\\x0000e8s_content\\=\\x00263a \\; the symbol of hapiness"));
    let dumped = val.dump();

    assert_eq!(dumped, "tr\\x0000e8s_content\\=\\x00263a \\; the symbol of hapiness");
}

#[test]
fn value_parse_raw() {
    let val = Value::parse(r"Hello \x002665").unwrap();

    assert_eq!(val, Value::Raw(String::from("Hello \u{2665}")));
}

#[test]
fn value_parse_str_ok() {
    let val = Value::parse(r"'Hello world \x00263a'").unwrap();

    assert_eq!(val, Value::Str(String::from("Hello world \u{263a}")));
}

#[test]
fn value_parse_str_unclosed() {
    match Value::parse("'Hello world") {
        Ok(_)                        => panic!("This value is invalid and should not be accepted"),
        Err(Error::ExpectedToken(_)) => {},
        Err(err)                     => panic!("Invalid error value {:?}", err),
    }
}

#[test]
fn value_parse_int() {
    let val = Value::parse("666").unwrap();

    assert_eq!(val, Value::Int(666));
}

#[test]
fn value_parse_float() {
    let val = Value::parse("666.0").unwrap();

    assert_eq!(val, Value::Float(666.0));
}

#[test]
fn value_parse_bool_on() {
    let val = Value::parse("on").unwrap();

    assert_eq!(val, Value::Bool(true));
}

#[test]
fn value_parse_bool_enabled() {
    let val = Value::parse("enabled").unwrap();

    assert_eq!(val, Value::Bool(true));
}

#[test]
fn value_parse_bool_y() {
    let val = Value::parse("y").unwrap();

    assert_eq!(val, Value::Bool(true));
}

#[test]
fn value_parse_bool_yes() {
    let val = Value::parse("yes").unwrap();

    assert_eq!(val, Value::Bool(true));
}

#[test]
fn value_parse_bool_off() {
    let val = Value::parse("off").unwrap();

    assert_eq!(val, Value::Bool(false));
}

#[test]
fn value_parse_bool_disabled() {
    let val = Value::parse("disabled").unwrap();

    assert_eq!(val, Value::Bool(false));
}

#[test]
fn value_parse_bool_n() {
    let val = Value::parse("n").unwrap();

    assert_eq!(val, Value::Bool(false));
}

#[test]
fn value_parse_bool_no() {
    let val = Value::parse("no").unwrap();

    assert_eq!(val, Value::Bool(false));
}

#[test]
fn value_parse_err() {
    let val = Value::parse(r"Hello \p");

    assert!(val.is_err());
}

#[test]
fn identifier_new_some() {
    let section = Some(String::from("Section_name"));
    let variable = String::from("Variable_name");
    let ident = Identifier::new(section.clone(), variable.clone());

    assert_eq!(ident, Identifier { section, name: variable });
}

#[test]
fn identifier_new_none() {
    let section = None;
    let variable = String::from("Variable_name");
    let ident = Identifier::new(section.clone(), variable.clone());

    assert_eq!(ident, Identifier { section, name: variable });
}

#[test]
#[should_panic]
fn identifier_new_panics() {
    let section = Some(String::from("-Bad section name"));
    let variable = String::from("regular_name");
    let _ident = Identifier::new(section, variable);
}

#[test]
fn identifier_is_valid_full_test() {
    assert!(Identifier::is_valid("UPPERCASE_ONE"));
    assert!(Identifier::is_valid("lowercase_one"));
    assert!(Identifier::is_valid("alpha_numeric_42"));
    assert!(Identifier::is_valid("Contains spaces"));
    assert!(Identifier::is_valid("$is-valid_since:version~1.2.0"));
    assert!(!Identifier::is_valid("42_starts_with_a_digit"));
    assert!(!Identifier::is_valid("Non_ascii_character_\u{263a}"));
    assert!(!Identifier::is_valid("invalid_character="));
    assert!(!Identifier::is_valid(""));
}

#[test]
fn identifier_change_section_ok() {
    let mut ident = Identifier::new(Some(String::from("Section")), String::from("Variable"));

    ident.change_section(Some(String::from("Valid_one")));
}

#[test]
#[should_panic]
fn identifier_change_section_err() {
    let mut ident = Identifier::new(Some(String::from("Section")), String::from("Variable"));

    ident.change_section(Some(String::from("Inv@lid one")));
}

#[test]
fn identifier_change_section_none() {
    let mut ident = Identifier::new(Some(String::from("Section")), String::from("Variable"));

    ident.change_section(None);
}

#[test]
fn identifier_change_name_ok() {
    let mut ident = Identifier::new(Some(String::from("Section")), String::from("Variable"));

    ident.change_name(String::from("Valid_one"));
}

#[test]
#[should_panic]
fn identifier_change_name_err() {
    let mut ident = Identifier::new(Some(String::from("Section")), String::from("Variable"));

    ident.change_name(String::from("Inv@lid one"));
}

#[test]
fn identifier_format_with_section() {
    let section = String::from("Section");
    let variable = String::from("Variable");
    let ident = Identifier::new(Some(section.clone()), variable.clone());

    assert_eq!(format!("{}", ident), format!("{}.{}", section, variable));
}

#[test]
fn identifier_format_without_section() {
    let section = None;
    let variable = String::from("Variable");
    let ident = Identifier::new(section, variable.clone());

    assert_eq!(format!("{}", ident), variable);
}



================================================
File: mininip/src/datas/tree/mod.rs
================================================
//! An higher-level representation API for the data returned by parsers
//! 
//! # See
//! `Tree` to convert a `HashMap<Identifier, Value>` into a more user-friendly data-type
//! 
//! `Section` to list the keys inside a section

use crate::datas::{Identifier, Value};
use std::collections::{HashMap, hash_map};

/// A more user-friendly data-type to represent the data returned by `parser::Parser::data`
/// 
/// # Example
/// ```
/// use mininip::datas{Identifier, Value, self};
/// use datas::tree::Tree;
/// use mininip::parse::parse_file;
/// 
/// let tree = Tree::from_data(parse_file("good.ini").unwrap());
/// for i in tree.sections() {
///     println!("[{}] ; Section {}", i, i);
///     for j in i.keys() {
///         println!("{}={} ; key {}", j.ident().name(), j.value(), j.ident().name());
///     }
/// }
/// ```
pub struct Tree {
    cache: Cache,
    data: HashMap<Identifier, Value>,
}

impl Tree {
    /// Iterates over the sections of a `Tree`
    pub fn sections(&self) -> SectionIterator<'_> {
        SectionIterator {
            iterator: self.cache.sections.iter(),
            target: self,
            awaited: false,
        }
    }

    /// Returns an immutable reference to the owned data
    pub fn get_data(&self) -> &HashMap<Identifier, Value> {
        &self.data
    }

    /// Consumes `self` and returns the owned data
    pub fn into_data(self) -> HashMap<Identifier, Value> {
        self.data
    }
}

impl From<HashMap<Identifier, Value>> for Tree {
    fn from(data: HashMap<Identifier, Value>) -> Tree {
        Tree {
            cache: Cache::from(&data),
            data: data,
        }
    }
}


/// An iterator over sections in a `Tree`
pub struct SectionIterator<'a> {
    /// An iterator over the sections names in the `Tree`
    iterator: std::slice::Iter<'a, String>,
    /// The `Tree` owning the data iterated
    target: &'a Tree,
    /// Set to `true` if already awaited, `false` otherwise. Necessary because
    /// if it is set to `false`, we need to iterate over the global section
    /// before the named ones
    awaited: bool,
}

impl<'a> Iterator for SectionIterator<'a> {
    type Item = Section<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        if !self.awaited {
            self.awaited = true;

            if self.target.cache.keys.get(&None).is_some() {
                return Some(Section {
                    ident: None,
                    target: self.target,
                });
            }
        }

        let ident = self.iterator.next()?;
        Some(Section {
            ident: Some(&ident),
            target: self.target,
        })
    }
}


/// A section in a `Tree`
pub struct Section<'a> {
    ident: Option<&'a str>,
    target: &'a Tree,
}

impl<'a> Section<'a> {
    /// Returns the identifier (name) of this section
    pub fn name(&self) -> Option<&'a str> {
        self.ident.clone()
    }

    /// Returns an iterator over the keys of this section
    pub fn keys(&self) -> KeyIterator<'_> {
        KeyIterator {
            iterator: self.keys_internal_iterator(),
            target: self,
        }
    }

    /// Returns an iterator ofer the keys of this section.
    /// 
    /// # Note
    /// This iterator is the one internally used by the vector storing these key
    /// names. It must not be exposed in a public interface
    fn keys_internal_iterator(&self) -> std::slice::Iter<'a, String> {
        self.target.cache.keys[&self.name_owned()].iter()
    }

    /// Returns the identifier of this section like it must be passed to an
    /// `Identifier`: an `Option<String>` instead of an `Option<&str>`
    pub fn name_owned(&self) -> Option<String> {
        match self.ident {
            Some(val) => Some(String::from(val)),
            None      => None,
        }
    }
}


/// An iterator over keys in a given section
pub struct KeyIterator<'a> {
    iterator: std::slice::Iter<'a, String>,
    target: &'a Section<'a>,
}

impl<'a> Iterator for KeyIterator<'a> {
    type Item = Identifier;

    fn next(&mut self) -> Option<Self::Item> {
        let key = self.iterator.next()?;
        let section = self.target.name_owned();

        Some(Identifier::new(section, key.clone()))
    }
}


/// A cached result of an extraction of all the section and keys names. Will be
/// kept and updated forever in the owning `Tree`
struct Cache {
    /// An ordered list of sections
    sections: Vec<String>,
    /// A map associating a section name to an ordered list of key names
    keys: HashMap<Option<String>, Vec<String>>,
}

impl From<&HashMap<Identifier, Value>> for Cache {
    fn from(data: &HashMap<Identifier, Value>) -> Cache {
        let mut sections = Vec::new();
        let mut keys = HashMap::<_, Vec<String>>::new();

        for i in data.keys() {
            let section_name = match i.section() {
                Some(val) => Some(String::from(val)),
                None      => None,
            };

            match keys.entry(section_name.clone()) {
                hash_map::Entry::Occupied(mut entry) => entry.get_mut().push(String::from(i.name())),
                hash_map::Entry::Vacant(entry)       => {
                    let vec = vec![String::from(i.name())];
                    entry.insert(vec);

                    if let Some(val) = section_name {
                        sections.push(val);
                    }
                },
            }
        }

        // No collisions so unstable sorting is more efficient
        sections.sort_unstable();

        if let Some(val) = keys.get_mut(&None) {
            val.sort_unstable();
        }
        for i in &sections {
            keys.get_mut(&Some(i.clone()))
                .expect("Any section name in `section` should be in `keys`")
                .sort_unstable();
        }

        Cache {
            sections,
            keys,
        }
    }
}


#[cfg(test)]
mod tests;



================================================
File: mininip/src/datas/tree/tests.rs
================================================
use crate::datas::{tree::*, Identifier, Value};
use crate::parse::Parser;

#[test]
fn cache_from_data() {
    let mut data = HashMap::new();

    let section = None;
    data.insert(Identifier::new(section.clone(), String::from("version")), Value::Str(String::from("1.3.0")));
    data.insert(Identifier::new(section.clone(), String::from("debug")), Value::Bool(true));
    data.insert(Identifier::new(section,         String::from("allow-errors")), Value::Bool(false));

    let section = Some(String::from("foo"));
    data.insert(Identifier::new(section.clone(), String::from("answer")), Value::Int(42));
    data.insert(Identifier::new(section,         String::from("pi")), Value::Float(3.14));

    let section = Some(String::from("bar"));
    data.insert(Identifier::new(section.clone(), String::from("baz")), Value::Raw(String::new()));
    data.insert(Identifier::new(section,         String::from("abc")), Value::Str(String::from("def")));

    let cache = Cache::from(&data);
    assert_eq!(&cache.sections, &vec![String::from("bar"), String::from("foo")]);

    let global = &cache.keys[&None];
    assert_eq!(global, &vec![String::from("allow-errors"), String::from("debug"), String::from("version")]);

    let foo = &cache.keys[&Some(String::from("foo"))];
    assert_eq!(foo, &vec![String::from("answer"), String::from("pi")]);

    let bar = &cache.keys[&Some(String::from("bar"))];
    assert_eq!(bar, &vec![String::from("abc"), String::from("baz")]);
}

#[test]
fn section_iterator_iterates_well() {
    // Here, we assume that the parser and the `Tree`'s constructor works well
    let mut parser = Parser::new();
    let content = "\
    version = '1.3.0'\n\
    debug = y\n\
    allow-errors = y\n\
    \n\
    [foo]\n\
    answer = 42\n\
    pi = 3.14\n\
    \n\
    [bar]\n\
    baz =\n\
    abc = \"def\"\n\
    ";

    for i in content.lines() {
        parser.parse_line(i)
            .expect("This code is valid");
    }

    let tree = Tree::from(parser.data());
    let expected = [None, Some("bar"), Some("foo")];

    for (n, i) in tree.sections().enumerate() {
        assert_eq!(&i.name(), &expected[n]);
    }
}

#[test]
fn key_iterator_iterates_well() {
    let mut data = HashMap::new();

    let section = None;
    data.insert(Identifier::new(section.clone(), String::from("version")), Value::Str(String::from("1.3.0")));
    data.insert(Identifier::new(section.clone(), String::from("debug")), Value::Bool(true));
    data.insert(Identifier::new(section,         String::from("allow-errors")), Value::Bool(false));

    let tree = Tree::from(data);
    let global = tree.sections()
                     .next()
                     .expect("This tree only owns one section");

    let expected = ["allow-errors", "debug", "version"];
    for (n, i) in global.keys().enumerate() {
        assert_eq!(i.name(), expected[n]);
        assert_eq!(i.section(), None);
    }
}

#[test]
fn key_iterator_no_global() {
    let mut data = HashMap::new();

    let section = Some(String::from("foo"));
    data.insert(Identifier::new(section.clone(), String::from("version")), Value::Str(String::from("1.3.0")));
    data.insert(Identifier::new(section.clone(), String::from("debug")), Value::Bool(true));
    data.insert(Identifier::new(section,         String::from("allow-errors")), Value::Bool(false));

    let tree = Tree::from(data);
    let foo = tree.sections()
                  .next()
                  .expect("There is one single section in this tree");

    assert_eq!(foo.name(), Some("foo"));
}



================================================
File: mininip/src/dump/mod.rs
================================================
//! Provides tools to generate a INI file from any data

/// Formats a `&str` by escaping special characters
/// 
/// # Return value
/// A `String` containing the escaped string
/// 
/// # Why should I format it?
/// The `Display` trait is about displaying a value to the user while `Debug` is for debuging. There is not any trait for dumping a value in a file knowing it can't be backed up in the same way it is displayed, so `escape` does this.
/// 
/// For instance, if `content` is `"a'bc=123;"`, then, `escape` will return `r"a\'bc\=123\;"` because it escapes special characters such as `=`, `'`, `;`, ...
/// 
/// More escaped characters may be found at [Wikipedia](https://en.wikipedia.org/wiki/INI_file#Escape_characters "INI file")
/// 
/// # The Unicode special case
/// A non-ASCII character is escaped as a `\x??????` with exactly 6 hexadecimal digits even if a smaller number is suitable
/// 
/// # Examples
/// ```
/// use mininip::dump::dump_str;
/// 
/// assert_eq!(dump_str("a'bc=123;"), r"a\'bc\=123\;");
/// assert_eq!(dump_str("\u{263a}"),  r"\x00263a");
/// ```
pub fn dump_str(content: &str) -> String {
    let mut new = String::with_capacity(content.len());

    for i in content.chars() {
        match i {
            // Those characters have a special rule to be escaped
            '\\'   => new.push_str(r"\\"),
            '\''   => new.push_str("\\'"),
            '"'    => new.push_str("\\\""),
            '\0'   => new.push_str("\\0"),
            '\x07' => new.push_str("\\a"),
            '\x08' => new.push_str("\\b"),
            '\t'   => new.push_str("\\t"),
            '\r'   => new.push_str("\\r"),
            '\n'   => new.push_str("\\n"),
            ';'    => new.push_str("\\;"),
            '#'    => new.push_str("\\#"),
            '='    => new.push_str("\\="),
            ':'    => new.push_str("\\:"),

            // The ASCII characters are left unchanged
            _ => new.push(i),
        }
    }

    new
}


mod dumper;
pub use dumper::*;

#[cfg(test)]
mod tests;



================================================
File: mininip/src/dump/tests.rs
================================================
use crate::dump::*;

/// Tests only the constant substitutions such as `\` -> `\\` and not the runtime-computed ones
#[test]
fn dump_str_constants_substitutions() {
    assert_eq!(dump_str("\\"),   r"\\");
    assert_eq!(dump_str("'"),    String::from("\\'"));
    assert_eq!(dump_str("\""),   String::from("\\\""));
    assert_eq!(dump_str("\0"),   String::from("\\0"));
    assert_eq!(dump_str("\x07"), String::from("\\a"));
    assert_eq!(dump_str("\x08"), String::from("\\b"));
    assert_eq!(dump_str("\t"),   String::from("\\t"));
    assert_eq!(dump_str("\r"),   String::from("\\r"));
    assert_eq!(dump_str("\n"),   String::from("\\n"));
    assert_eq!(dump_str(";"),    String::from("\\;"));
    assert_eq!(dump_str("#"),    String::from("\\#"));
    assert_eq!(dump_str("="),    String::from("\\="));
    assert_eq!(dump_str(":"),    String::from("\\:"));
}

#[test]
fn dump_str_dynamic_substitutions() {
    assert_eq!(dump_str("\u{00263a}"), String::from("\\x00263a"));
    assert_eq!(dump_str("\u{000100}"), String::from("\\x000100"));
    assert_eq!(dump_str("\u{01342e}"), String::from("\\x01342e"));
}

#[test]
fn dump_str_ignore() {
    assert_eq!(dump_str("abc123"), String::from("abc123"));
}

#[test]
fn dump_str_complementary_test() {
    assert_eq!(dump_str("très_content=☺ ; the symbol of hapiness"), "tr\\x0000e8s_content\\=\\x00263a \\; the symbol of hapiness");
}




================================================
File: mininip/src/dump/dumper/mod.rs
================================================
//! Provides a `Dumper` structure which creates a new INI file content

use crate::datas::{Identifier, Value};
use std::collections::{hash_map, HashMap};
use std::path::Path;
use std::fs::File;
use std::io::{self, Write};

/// A stated object, which from couples of [`Identifier`](../datas/struct.Identifier.html "datas::Identifier") and [`Value`](../datas/enum.Value.html "datas::Value"), creates a new INI tree, directly dumpable into a new file
/// 
/// # Example
/// ```
/// use mininip::dump::Dumper;
/// use mininip::datas::{Identifier, Value};
/// 
/// let mut dumper = Dumper::new();
/// 
/// let section = None;
/// let name = String::from("abc");
/// let abc = Identifier::new(section, name);
/// let value = Value::Raw(String::from("happy = \u{263a}"));
/// 
/// dumper.dump(abc, value);
/// 
/// let section = Some(String::from("maths"));
/// let name = String::from("sum");
/// let sum = Identifier::new(section, name);
/// let value = Value::Raw(String::from("\u{3a3}"));
/// 
/// dumper.dump(sum, value);
/// 
/// let expected = "\
/// abc=happy \\= \\x00263a\n\
/// \n\
/// [maths]\n\
/// sum=\\x0003a3\n";
/// 
/// assert_eq!(dumper.generate(), expected);
/// ```
#[derive(Debug)]
pub struct Dumper {
    /// The keys of this member are the section names and the values are a list of affectation lines generated
    tree: HashMap<Option<String>, Vec<String>>,
}

impl Dumper {
    /// Creates a new `Dumper` object
    pub fn new() -> Dumper {
        Dumper {
            tree: HashMap::new(),
        }
    }

    /// Dumps a couple [`Identifier`](../datas/struct.Identifier.html "datas::Identifier") / [`Value`](../datas/enum.Value.html "datas::Value") into `self`
    pub fn dump(&mut self, identifier: Identifier, value: Value) {
        let line = format!("{}={}", identifier.name(), value.dump());

        let key = match identifier.section() {
            Some(val) => Some(String::from(val)),
            None      => None,
        };
        match self.tree.entry(key) {
            hash_map::Entry::Occupied(mut entry) => entry.get_mut().push(line),
            hash_map::Entry::Vacant(entry)       => { entry.insert(vec![line]); },
        }
    }

    /// Generates a `String` containing the code of the INI data stored in the `Dumper`
    pub fn generate(mut self) -> String {
        // We want the sections to be sorted by name
        let mut sections: Vec<String> = Vec::with_capacity(self.tree.len());
        for (key, _value) in self.tree.iter() {
            if let Some(val) = key {
                sections.push(val.clone());
            }
        }
        sections.sort();

        // And None to be the first one
        let mut result = String::new();
        if let Some(val) = self.tree.get_mut(&None) {
            val.sort();
            for i in val {
                result.push_str(i);
                result.push('\n');
            }

            result.push('\n');
        }

        for i in sections {
            result.push('[');
            result.push_str(&i);
            result.push_str("]\n");

            let section = self.tree.get_mut(&Some(i))
                                   .expect("i is in sections so it is valid");
            section.sort();
            for j in section {
                result.push_str(j);
                result.push('\n');
            }

            result.push('\n');
        }

        result.pop();
        result
    }
}

/// Dumps a `HashMap<Identifier, Value>` into a file
/// 
/// # Parameters
/// `path` the path of the file (must be closed)
/// 
/// `data` the data to dump
/// 
/// # Return value
/// Since any [`Dumper`](struct.Dumper.html "dump::Dumper") operation is infallible, it only returns an `io::Result<()>` which indicates a file manipulation error
pub fn dump_into_file<T: AsRef<Path>>(path: T, data: HashMap<Identifier, Value>) -> io::Result<()> {
    let mut file = File::create(path)?;
    let mut dumper = Dumper::new();

    for (k, v) in data {
        dumper.dump(k, v);
    }

    file.write(dumper.generate().as_bytes())?;
    Ok(())
}


#[cfg(test)]
mod tests;



================================================
File: mininip/src/dump/dumper/tests.rs
================================================
use crate::dump::dumper::*;
use crate::datas::{Identifier, Value};

#[test]
fn dumper_without_globals() {
    let mut dumper = Dumper::new();

    let abc = Some(String::from("abc"));
    let a = Identifier::new(abc.clone(), String::from("a"));
    let b = Identifier::new(abc.clone(), String::from("b"));
    let c = Identifier::new(abc,         String::from("c"));

    let def = Some(String::from("def"));
    let d = Identifier::new(def.clone(), String::from("d"));
    let e = Identifier::new(def.clone(), String::from("e"));
    let f = Identifier::new(def,         String::from("f"));

    dumper.dump(a, Value::Int(1));
    dumper.dump(b, Value::Float(3.1415926535));
    dumper.dump(c, Value::Bool(true));
    dumper.dump(d, Value::Bool(false));
    dumper.dump(e, Value::Str(String::from("5")));
    dumper.dump(f, Value::Raw(String::from("abc")));

    let expected = "\
    [abc]\n\
    a=1\n\
    b=3.1415926535\n\
    c=on\n\
    \n\
    [def]\n\
    d=off\n\
    e='5'\n\
    f=abc\n";

    assert_eq!(expected, dumper.generate());
}

#[test]
fn dumper_with_globals() {
    let mut dumper = Dumper::new();

    let a = Identifier::new(None, String::from("a"));
    let b = Identifier::new(None, String::from("b"));
    let c = Identifier::new(None, String::from("c"));

    let def = Some(String::from("def"));
    let d = Identifier::new(def.clone(), String::from("d"));
    let e = Identifier::new(def.clone(), String::from("e"));
    let f = Identifier::new(def,         String::from("f"));

    dumper.dump(a, Value::Int(1));
    dumper.dump(b, Value::Float(3.1415926535));
    dumper.dump(c, Value::Bool(true));
    dumper.dump(d, Value::Bool(false));
    dumper.dump(e, Value::Str(String::from("5")));
    dumper.dump(f, Value::Raw(String::from("abc")));

    let expected = "\
    a=1\n\
    b=3.1415926535\n\
    c=on\n\
    \n\
    [def]\n\
    d=off\n\
    e='5'\n\
    f=abc\n";

    assert_eq!(expected, dumper.generate());
}

#[test]
fn dumper_with_escape() {
    let mut dumper = Dumper::new();

    let ident = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from(":D = \u{263a}"));

    dumper.dump(ident, val);

    assert_eq!("ident=\\:D \\= \\x00263a\n", dumper.generate());
}



================================================
File: mininip/src/errors/mod.rs
================================================
//! This module contains several error error types and their implementations

use std::error;
use std::fmt::{self, Display};
use std::io;

/// Represents a parsing error in the INI format
#[derive(Debug)]
pub enum Error {
    ExpectedIdentifier(error_kinds::ExpectedIdentifier),
    ExpectedToken(error_kinds::ExpectedToken),
    ExpectedEscape(error_kinds::ExpectedEscape),
    UnexpectedToken(error_kinds::UnexpectedToken),
    InvalidEscape(error_kinds::InvalidEscape),
    InvalidIdentifier(error_kinds::InvalidIdentifier),
}

impl error::Error for Error {}

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::ExpectedIdentifier(err) => write!(f, "{}", err),
            Error::ExpectedToken(err)      => write!(f, "{}", err),
            Error::ExpectedEscape(err)     => write!(f, "{}", err),
            Error::UnexpectedToken(err)    => write!(f, "{}", err),
            Error::InvalidEscape(err)      => write!(f, "{}", err),
            Error::InvalidIdentifier(err)  => write!(f, "{}", err),
        }
    }
}

impl From<error_kinds::ExpectedIdentifier> for Error {
    fn from(src: error_kinds::ExpectedIdentifier) -> Error {
        Error::ExpectedIdentifier(src)
    }
}

impl From<error_kinds::ExpectedToken> for Error {
    fn from(src: error_kinds::ExpectedToken) -> Error {
        Error::ExpectedToken(src)
    }
}

impl From<error_kinds::ExpectedEscape> for Error {
    fn from(src: error_kinds::ExpectedEscape) -> Error {
        Error::ExpectedEscape(src)
    }
}

impl From<error_kinds::UnexpectedToken> for Error {
    fn from(src: error_kinds::UnexpectedToken) -> Error {
        Error::UnexpectedToken(src)
    }
}

impl From<error_kinds::InvalidEscape> for Error {
    fn from(src: error_kinds::InvalidEscape) -> Error {
        Error::InvalidEscape(src)
    }
}

impl From<error_kinds::InvalidIdentifier> for Error {
    fn from(src: error_kinds::InvalidIdentifier) -> Error {
        Error::InvalidIdentifier(src)
    }
}

/// Contains all the error types used in `Error`'s variants
pub mod error_kinds {
    use std::error;
    use std::fmt::{self, Display};

    /// A parsing error happening when an identifier is expected but not found
    #[derive(Debug)]
    pub struct ExpectedIdentifier {
        index: usize,
        line: String,
    }

    impl error::Error for ExpectedIdentifier {}

    impl Display for ExpectedIdentifier {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Expected identifier {}{{here}}{}", &self.line[..self.index], &self.line[self.index..])
        }
    }

    impl ExpectedIdentifier {
        /// Creates a new `ExpectedIdentifier` error
        /// 
        /// # Parameters
        /// `line`: the line where the error occured. Should be complete
        /// 
        /// `index`: the index where the identifier is expected
        /// 
        /// # Panics
        /// Panics if index is too big
        pub fn new(line: String, index: usize) -> ExpectedIdentifier {
            assert!(line.len() >= index, "`index` must be a valid index in `line`");

            ExpectedIdentifier {
                line,
                index,
            }
        }
    }

    /// A parsing error happening when an arbitrary token is expected but not found
    #[derive(Debug)]
    pub struct ExpectedToken {
        index: usize,
        line: String,
        tokens: String,
    }

    impl error::Error for ExpectedToken {}

    impl Display for ExpectedToken {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Expected {} {}{{here}}{}", self.tokens, &self.line[..self.index], &self.line[self.index..])
        }
    }

    impl ExpectedToken {
        /// Creates a new `ExpectedToken` error
        /// 
        /// # Parameters
        /// `line`: the line where the error occured. Should be complete
        /// 
        /// `index`: the index where the token is expected
        /// 
        /// `tokens`: the possible tokens. There is no rule to format it, you just should be aware this will be printed directly to the end user
        /// 
        /// # Panics
        /// Panics if `index` is too big
        pub fn new(line: String, index: usize, tokens: String) -> ExpectedToken {
            assert!(line.len() >= index, "`index` must be a valid index");

            ExpectedToken {
                line,
                index,
                tokens,
            }
        }
    }

    /// A parsing error happening when a character should be escaped but is not
    /// 
    /// # See
    /// See [`dump_str`](../../dump/fn.dump_str.html "dump::dump_str") for more informations about escape sequences
    #[derive(Debug)]
    pub struct ExpectedEscape {
        index: usize,
        line: String,
        replace: String,
        token: char,
    }

    impl error::Error for ExpectedEscape {}

    impl Display for ExpectedEscape {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Expected escape sequence {} instead of {} in {}{{here}}{}", 
                       self.replace,
                       self.token,
                       &self.line[..self.index],
                       &self.line[self.index + self.token.len_utf8()..])
        }
    }

    impl ExpectedEscape {
        /// Creates a new `ExpectedEscape` error
        /// 
        /// # Parameters
        /// `line`: the line where the error occured
        /// 
        /// `index`: the index of the error
        /// 
        /// `replace`: the escape sequence which should be used instead
        /// 
        /// # Panics
        /// Panics if `index` is too big or is at an invalid position
        pub fn new(line: String, index: usize, replace: String) -> ExpectedEscape {
            ExpectedEscape {
                token: super::nth_char(&line, index),
                line,
                replace,
                index,
            }
        }
    }

    /// A parsing error happening when an arbitrary token is found where it should not
    #[derive(Debug)]
    pub struct UnexpectedToken {
        index: usize,
        line: String,
        token: char,
    }

    impl error::Error for UnexpectedToken {}

    impl Display for UnexpectedToken {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Unexpected token {} {}{{here}}",
                       self.token,
                       &self.line[..self.index])
        }
    }

    impl UnexpectedToken {
        /// Creates a new `UnexpectedToken` error
        /// 
        /// # Parameters
        /// `line`: the line where the error occured
        /// 
        /// `index`: the index where a token was not expected
        /// 
        /// # Panics
        /// Panics if `index` is too big or is at an invalid position
        pub fn new(line: String, index: usize) -> UnexpectedToken {
            UnexpectedToken {
                index,
                token: super::nth_char(&line, index),
                line,
            }
        }
    }

    /// A parsing error happening when an escape sequence is not recognised
    /// 
    /// # See
    /// See [`dump_str`](../../dump/fn.dump_str.html "dump::dump_str") for more informations about escape sequences
    #[derive(Debug)]
    pub struct InvalidEscape {
        line: String,
        escape: String,
    }

    impl error::Error for InvalidEscape {}

    impl Display for InvalidEscape {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Invalid escape sequence {} in {}", self.escape, self.line)
        }
    }

    impl InvalidEscape {
        /// Creates a new `InvalidEscape` error
        /// 
        /// # Parameters
        /// `line`: the line where the error occured
        /// 
        /// `escape`: the escape sequence which is invalid
        /// 
        /// # Panics
        /// Panics if `escape` is not in `line`
        pub fn new(line: String, escape: String) -> InvalidEscape {
            assert!(line.find(&escape).is_some(), "`line` must contain `escape`");

            InvalidEscape {
                line,
                escape,
            }
        }
    }

    /// A parsing error happening when an identifier is expected but the expression found is not a valid identifier
    /// 
    /// # See
    /// See [`Identifier::is_valid`](../../datas/struct.Identifier.html#method.is_valid "datas::Identifier::is_valid") to know what is defined as a valid or invalid identifier according to the INI format
    #[derive(Debug)]
    pub struct InvalidIdentifier {
        line: String,
        ident: String,
    }

    impl error::Error for InvalidIdentifier {}

    impl Display for InvalidIdentifier {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Invalid identifier {} in {}", self.ident, self.line)
        }
    }

    impl InvalidIdentifier {
        /// Creates a new `InvalidIdentifier` error
        /// 
        /// # Parameters
        /// `line`: the line where the error occured
        /// 
        /// `identifier`: the identifier found. It must be invalid
        /// 
        /// # Panics
        /// Panics
        /// - if `identifier` is valid
        /// - if `identifier` is not in `line`
        pub fn new(line: String, identifier: String) -> InvalidIdentifier {
            assert!(line.find(&identifier).is_some(), "`line` must contain `identifier`");
            assert!(!crate::datas::Identifier::is_valid(&identifier), "`identifier` must be an invalid identifier");

            InvalidIdentifier {
                line,
                ident: identifier,
            }
        }
    }
}

/// Represents either an IO error or a parsing error
/// 
/// Is used by this library in [`parse_file`](../parse/fn.parse_file.html "parse::parse_file") which may encounter an error with the file to parse or with its content
#[derive(Debug)]
pub enum ParseFileError {
    IOError(io::Error),
    ParseError(Error),
}

impl error::Error for ParseFileError {}

impl Display for ParseFileError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            ParseFileError::IOError(err)    => write!(f, "{}", err),
            ParseFileError::ParseError(err) => write!(f, "{}", err),
        }
    }
}

impl From<io::Error> for ParseFileError {
    fn from(err: io::Error) -> ParseFileError {
        ParseFileError::IOError(err)
    }
}

impl From<Error> for ParseFileError {
    fn from(err: Error) -> ParseFileError {
        ParseFileError::ParseError(err)
    }
}

/// Returns the character at the `index`th index (`index` is in bytes) in `string`
/// 
/// # Panics
/// Panics if `index` is out of range or between two bytes of the same character
fn nth_char(string: &str, index: usize) -> char {
    assert!(string.len() >= index, "`index` must be a valid index in `string`");

    let mut token = '\0';
    let mut last_n = 0;

    for (n, i) in string.char_indices() {
        last_n = n;

        if n == index {
            token = i;
            break;
        } else if n > index {
            panic!("`index` is not a valid index in `string` (`index` = {:?}, `string` = {:?})", index, string);
        }
    }

    assert_eq!(last_n, index, "`index` is not a valid index in `string` (`index` = {:?}, `string` = {:?})", index, string);

    token
}


#[cfg(test)]
mod tests;



================================================
File: mininip/src/errors/tests.rs
================================================
use crate::errors::*;

#[test]
fn expected_identifier_format() {
    let line = String::from("=hello");
    let err = error_kinds::ExpectedIdentifier::new(line, 0);

    let fmt = format!("{}", err);
    assert_eq!(fmt, "Expected identifier {here}=hello");
}

#[test]
fn expected_token_format() {
    let line = String::from("hello world");
    let err = error_kinds::ExpectedToken::new(line, 5, String::from("="));

    let fmt = format!("{}", err);
    assert_eq!(fmt, "Expected = hello{here} world");
}

#[test]
fn expected_escape_format() {
    let line = String::from("greet = hello \u{263a}");
    let err = error_kinds::ExpectedEscape::new(line, 14, String::from("\\x00263a"));

    let fmt = format!("{}", err);
    assert_eq!(fmt, "Expected escape sequence \\x00263a instead of \u{263a} in greet = hello {here}");
}

#[test]
fn unexpected_token_format() {
    let line = String::from("ident\\x002665 = value");
    let err = error_kinds::UnexpectedToken::new(line, 5);

    let fmt = format!("{}", err);
    assert_eq!(fmt, "Unexpected token \\ ident{here}");
}

#[test]
fn invalid_escape_format() {
    let line = String::from("ident=\\xyzabcd");
    let err = error_kinds::InvalidEscape::new(line, String::from("\\xyzabcd"));

    let fmt = format!("{}", err);
    assert_eq!(fmt, "Invalid escape sequence \\xyzabcd in ident=\\xyzabcd");
}

#[test]
#[should_panic]
fn expected_identifier_overflow() {
    let line = String::from("[]; a non-named section");
    let _err = error_kinds::ExpectedIdentifier::new(line, 1_000_000);
}

#[test]
#[should_panic]
fn expected_token_overflow() {
    let line = String::from("hello world");
    let _err = error_kinds::ExpectedToken::new(line, 1_000_000, String::from("="));
}

#[test]
#[should_panic]
fn expected_escape_overflow() {
    let line = String::from("hello world");
    let _err = error_kinds::ExpectedEscape::new(line, 1_000_000, String::from("\\x00263a"));
}

#[test]
#[should_panic]
fn expected_escape_alignment_error() {
    let line = String::from("greet = hello \u{263a}");
    // 15 is not an overflow but it's the second byte of ☺ (the last character)
    let _err = error_kinds::ExpectedEscape::new(line, 15, String::from("\\x00263a"));
}

#[test]
#[should_panic]
fn unexpected_token_overflow() {
    let line = String::from("hello world");
    let _err = error_kinds::UnexpectedToken::new(line, 1000_000);
}

#[test]
#[should_panic]
fn unexpected_token_alignment_error() {
    let line = String::from("greet = hello \u{263a}");
    // 15 is not an overflow but it's the second byte of ☺ (the last character)
    let _err = error_kinds::UnexpectedToken::new(line, 15);
}

#[test]
#[should_panic]
fn invalid_escape_not_included() {
    let line = String::from("ident=\\xyzabcd");
    let _err = error_kinds::InvalidEscape::new(line, String::from("\\{"));
}

#[test]
fn nth_char_works_well() {
    assert_eq!(nth_char("abcdefg", 1), 'b');
}

#[test]
#[should_panic]
fn nth_char_alignment_error_middle() {
    let _char = nth_char("hello \u{263a} world", 7);
}

#[test]
#[should_panic]
fn nth_char_alignment_error_end() {
    let _char = nth_char("hello \u{263a}", 7);
}

#[test]
#[should_panic]
fn nth_char_overflow() {
    let _char = nth_char("hello", 1_000_000);
}



================================================
File: mininip/src/parse/mod.rs
================================================
//! Provides tools to parse an INI file

use crate::errors::{error_kinds::*, Error};
use std::iter::Fuse;

/// Reads a string formatted by [`dump_str`](../dump/fn.dump_str.html "dump::dump_str") and unescapes the escaped characters
///
/// # Return value
/// `Ok(string)` with `string` as the result once parsed
///
/// `Err(err)` In case of error with `err` as the error code
///
/// # Encoding issues
/// Only allows ASCII because Unicode or other encodings musn't appear in an INI file (except in comments but this function is not intended to parse whole files)
///
/// # Examples
/// ```
/// use mininip::parse::parse_str;
///
/// assert!(parse_str("Bad because ends with a ;").is_err());
/// assert_eq!(parse_str(r"abc\=123\; \x00263a").unwrap(), "abc=123; \u{263a}");
/// ```
pub fn parse_str(content: &str) -> Result<String, Error> {
    // new will never be wider than content

    let mut new = String::with_capacity(content.len());

    static FORBIDDEN: [char; 13] = [
        '\x07', '\x08', '\t', '\r', '\n', '\0', '\\', '\'', '\"', ';', ':', '=', '#',
    ];

    // `next` is the index (as bytes) of the next escape sequence in content
    let mut next = 0;
    for i in TokenIterator::from(content.chars()) {
        let escape = match i {
            Token::Char(c) => {
                let n = next;
                next += 1;

                if FORBIDDEN.contains(&c)
                /*|| !c.is_ascii() */
                {
                    let escape = crate::dump::dump_str(&format!("{}", c));
                    let err = Error::from(ExpectedEscape::new(String::from(content), n, escape));
                    return Err(err);
                }

                new.push(c);
                continue;
            }
            Token::Escape(s) => s,
        };

        next += escape.len();

        match escape.as_str() {
            "\\a" => new.push('\x07'),
            "\\b" => new.push('\x08'),
            "\\t" => new.push('\t'),
            "\\r" => new.push('\r'),
            "\\n" => new.push('\n'),
            "\\0" => new.push('\0'),
            r"\\" => new.push('\\'),
            "\\'" => new.push('\''),
            "\\\"" => new.push('\"'),
            "\\;" => new.push(';'),
            "\\:" => new.push(':'),
            "\\=" => new.push('='),
            "\\#" => new.push('#'),

            _ if escape.len() == 8 => {
                debug_assert!(escape.starts_with("\\x"));

                let values = &escape[2..];
                let code = match u32::from_str_radix(values, 16) {
                    Ok(val) => val,
                    Err(_) => {
                        return Err(Error::from(InvalidEscape::new(
                            String::from(content),
                            escape,
                        )))
                    }
                };
                let character = match std::char::from_u32(code) {
                    Some(val) => val,
                    None => {
                        return Err(Error::from(InvalidEscape::new(
                            String::from(content),
                            escape,
                        )))
                    }
                };
                new.push(character);
            }

            _ => {
                return Err(Error::from(InvalidEscape::new(
                    String::from(content),
                    escape,
                )))
            }
        }
    }

    Ok(new)
}

/// A token which is either a single character or an escape sequence starting with `\`
#[derive(PartialEq, Debug)]
enum Token {
    Char(char),
    Escape(String),
}

/// An iterator over the characters of an INI file
///
/// Yields `Token`s which can be either a character or an escape sequence
///
/// # Safety
/// These characters are NOT TRUSTED, for example, you may receive a `\é` sequence wich is illegal in INI
///
/// If an escape sequence is left unfinished, it is returned as is in a `Token::Escape` object, even though it is invalid
struct TokenIterator<T> {
    iterator: Fuse<T>,
}

impl<T: Iterator> From<T> for TokenIterator<T> {
    fn from(iterator: T) -> TokenIterator<T> {
        TokenIterator {
            iterator: iterator.fuse(),
        }
    }
}

impl<T: Iterator<Item = char>> Iterator for TokenIterator<T> {
    type Item = Token;

    fn next(&mut self) -> Option<Token> {
        let mut escape_seq = String::with_capacity(8);

        loop {
            let i = match self.iterator.next() {
                Some(val) => val,

                // When the iterator returns `None`, we return the escape sequence if unfinished or `None` if the text was not escaped
                None if escape_seq.is_empty() => return None,
                None => return Some(Token::Escape(escape_seq)),
            };

            if !escape_seq.is_empty() {
                escape_seq.push(i);
            } else if i == '\\' {
                escape_seq.push(i);
                continue;
            } else {
                return Some(Token::Char(i));
            }

            if escape_seq.starts_with(r"\x") && escape_seq.len() < 8 {
                continue;
            }

            return Some(Token::Escape(escape_seq));
        }
    }
}

/// Finds the first non-escaped occurence of `pattern` in `string`
/// . Currently only accepts `char`s
///
/// # Return value
/// `Some(index)` with `index` as the index of the first occurence of `pattern`
///
/// `None` if `pattern` could not be found as a non-escaped form
pub fn find_unescaped(string: &str, pattern: char) -> Option<usize> {
    // possible values of `escape`
    // -1   : the last character parsed is a '\\'
    // 0    : this character must be read because it's unescaped
    // 1..6 : this character must be ignored because it belongs to an escape sequence
    let mut escape = 0;
    for (n, i) in string.char_indices() {
        if escape == -1 {
            escape = if i == 'x' { 6 } else { 0 };
        } else if escape > 0 {
            escape -= 1;
        }
        // Since here, escape = 0 so the character must be parsed
        else if i == '\\' {
            escape = -1;
        } else if i == pattern {
            return Some(n);
        }
    }

    None
}

mod parser;
pub use parser::*;

#[cfg(test)]
mod tests;



================================================
File: mininip/src/parse/tests.rs
================================================
use crate::parse::*;
use crate::errors::Error;

#[test]
fn token_iterator_no_escapes() {
    let message = "Hello world!";
    let found = TokenIterator::from(message.chars())
                .collect::<Vec<Token>>();

    let expected = message.chars()
                          .map(|c| Token::Char(c))
                          .collect::<Vec<Token>>();

    assert_eq!(found, expected);
}

#[test]
fn token_iterator_special_escapes() {
    let message = "\\;\\:\\=\\\\\\\'\\\"";
    let found = TokenIterator::from(message.chars())
                .collect::<Vec<Token>>();

    let expected = [r"\;", r"\:", r"\=", r"\\", r"\'", "\\\""]
                   .iter()
                   .map(|s| Token::Escape(String::from(*s)))
                   .collect::<Vec<Token>>();

    assert_eq!(found, expected);
}

#[test]
fn token_iterator_unicode_escapes() {
    let message = r"\x00263a\x002665\x000100";
    let found = TokenIterator::from(message.chars())
                .collect::<Vec<Token>>();

    let expected = [r"\x00263a", r"\x002665", r"\x000100"]
                   .iter()
                   .map(|s| Token::Escape(String::from(*s)))
                   .collect::<Vec<Token>>();

    assert_eq!(found, expected);
}

#[test]
fn token_iterator_unfinished_escape() {
    let message = r"Hello\";
    let found = TokenIterator::from(message.chars())
                .collect::<Vec<Token>>();

    let mut expected = message.chars()
                              .take(message.len() - 1)
                              .map(|c| Token::Char(c))
                              .collect::<Vec<Token>>();
    expected.push(Token::Escape(String::from("\\")));

    assert_eq!(found, expected);
}

#[test]
fn parse_str_ignore() {
    let message = "Hello world";

    assert_eq!(message, parse_str(message).expect("This string is well escaped"));
}

#[test]
fn parse_str_special_escapes() {
    let message = "\\a\\b\\;\\:\\=\\'\\\"\\t\\r\\n\\0\\\\\\#";
    let expected = "\x07\x08;:='\"\t\r\n\0\\#";

    assert_eq!(parse_str(message).expect("This string is well escaped"), expected);
}

#[test]
fn parse_str_unicode_escapes() {
    let message = r"\x00263a\x002665\x000100";
    let expected = "\u{263a}\u{2665}\u{100}";

    assert_eq!(parse_str(message).expect("This string is well escaped"), expected);
}

#[test]
fn parse_str_unfinished_escape() {
    let message = r"Hello\";

    match parse_str(message) {
        Ok(_)                        => panic!("This string is ill-escaped and shouldn't be accepted"),
        Err(Error::InvalidEscape(_)) => {},
        Err(err)                     => panic!("Wrong return value: {:?}", err),
    }
}

#[test]
fn parse_str_forbidden_ascii() {
    let message = r"hello=world";

    match parse_str(message) {
        Ok(_)                         => panic!("This string is ill-escaped and shouldn't be accepted"),
        Err(Error::ExpectedEscape(_)) => {},
        Err(err)                      => panic!("Wrong return value: {:?}", err),
    }
}

#[test]
fn parse_str_forbidden_unicode() {
    let message = "☺";

    match parse_str(message) {
        Ok(_)                         => panic!("This string is ill-escaped and shouldn't be accepted"),
        Err(Error::ExpectedEscape(_)) => {},
        Err(err)                      => panic!("Wrong return value: {:?}", err),
    }
}

#[test]
fn find_unescaped_found() {
    let sequence = "abc";

    assert_eq!(Some(2), find_unescaped(sequence, 'c'));
}

#[test]
fn find_unescaped_ignore_simple_escape() {
    let sequence = "ab\\;cd;";

    assert_eq!(Some(6), find_unescaped(sequence, ';'));
}

#[test]
fn find_unescaped_ignore_unicode_escape() {
    let sequence = "ab\\x00263a0";

    assert_eq!(Some(10), find_unescaped(sequence, '0'));
}

#[test]
fn find_unescaped_not_found() {
    let sequence = "abcd";

    assert_eq!(None, find_unescaped(sequence, 'e'));
}



================================================
File: mininip/src/parse/parser/mod.rs
================================================
//! Contains the definition of [`Parser`](struct.Parser.html "parse::Parser")

use std::collections::HashMap;
use crate::datas::{Identifier, Value};
use crate::errors::{Error, error_kinds::*, ParseFileError};
use std::path::Path;
use std::fs::File;
use std::io::Read;

/// A parser with a local state. Use it by passing it the text to parse line after line
/// 
/// # Notes
/// This parser does not work with a file but with lines passed as below. It allows you to parse an INI data from an iterator, a channel, the network...
/// # Examples
/// ```
/// use mininip::parse::Parser;
/// use mininip::datas::{Identifier, Value};
/// 
/// let mut parser = Parser::new();
/// 
/// parser.parse_line("abc = 123").unwrap();
/// parser.parse_line("; comment. A comment may start at an end of line").unwrap();
/// parser.parse_line("").unwrap(); // empty line
/// parser.parse_line("[section]").unwrap();
/// parser.parse_line("def = '\\;) \\= \\x00263a' ; This is perfectly valid").unwrap();
/// 
/// let data = parser.data();
/// 
/// let section = None;
/// let name = String::from("abc");
/// let abc = Identifier::new(section, name);
/// 
/// let value = Value::Int(123);
/// assert_eq!(data[&abc], value);
/// 
/// let section = Some(String::from("section"));
/// let name = String::from("def");
/// let def = Identifier::new(section, name);
/// 
/// let value = Value::Str(String::from(";) = \u{263a}"));
/// assert_eq!(data[&def], value);
/// ```
#[derive(Debug, Clone)]
pub struct Parser {
    variables: HashMap<Identifier, Value>,
    cur_section: Option<String>,
}

impl Parser {
    /// Creates a new `Parser`, which didn't parsed any line
    pub fn new() -> Parser {
        Parser {
            variables: HashMap::new(),
            cur_section: None,
        }
    }

    /// Consumes the parser and returns its data which is an `HashMap<Identifier, Value>` linking an identifier to its value
    pub fn data(self) -> HashMap<Identifier, Value> {
        self.variables
    }

    /// Parses a line
    /// 
    /// # Parameters
    /// `line` the line to parse
    /// 
    /// # Return value
    /// `Ok(())` in case of success
    /// 
    /// `Err(error)` in case of error with `error` as the error code (see [`Error`](../errors/enum.Error.html "errors::Error"))
    /// 
    /// # Examples
    /// ```rust
    /// use mininip::parse::Parser;
    /// use mininip::errors::{Error, error_kinds};
    /// use mininip::datas::{Identifier, Value};
    /// 
    /// let mut parser = Parser::new();
    /// 
    /// let good_line = "greeting = Hello \\x00263a";
    /// parser.parse_line(good_line)
    ///     .expect("This line is valid");
    /// 
    /// let bad_line = "how to greet? = Hello \\x00263a";
    /// match parser.parse_line(bad_line) {
    ///     Ok(())                             => panic!("This line is invalid and should not be accepted"),
    ///     Err(Error::InvalidIdentifier(err)) => assert_eq!(format!("{}", err), "Invalid identifier how to greet? in how to greet? = Hello \\x00263a"),
    ///     Err(err)                           => panic!("Wrong error returned (got {:?})", err),
    /// }
    /// ```
    pub fn parse_line(&mut self, line: &str) -> Result<(), Error> {
        let effective_line = line.trim_start();

        match effective_line.chars().next() {
            None | Some(';')    => Ok(()),
            Some(c) if c == '[' => self.parse_section(line),
            Some(_)             => self.parse_assignment(line),
        }
    }

    /// Parses an assignment ligne. An assignment is of form
    /// 
    /// ```ini
    /// identifier=value;comment
    /// ```
    /// 
    /// # Parameters
    /// `line` the line to parse
    /// 
    /// # Return value
    /// `Ok(())` in case of success
    /// 
    /// `Err(error)` in case of error with `error` as the error code
    fn parse_assignment(&mut self, line: &str) -> Result<(), Error> {
        // Getting the expression of `identifier` in "`identifier` = `value`[;comment]"
        let equal = match line.find('=') {
            Some(index) => index,
            None        => {
                let end_of_ident = line.trim_end().len();

                return Err(Error::from(ExpectedToken::new(String::from(line), end_of_ident, String::from("="))));
            }
        };

        let identifier = String::from(line[..equal].trim());

        // Getting the expression of `value` in "`identifier` = `value`[;comment]"
        let value = if line.len() == equal + 1 {
            ""
        } else {
            ignore_comment(&line[equal + 1..]).trim()
        };

        if !Identifier::is_valid(&identifier) {
            return Err(Error::from(InvalidIdentifier::new(String::from(line), identifier)));
        }
        let value = Value::parse(value)?;

        self.variables.insert(
            Identifier::new(self.cur_section.clone(), identifier),
            value,
        );
        Ok(())
    }

    /// Parses a section declaration. A section declaration is of form
    /// 
    /// ```ini
    /// [section];comment
    /// ```
    /// 
    /// # Parameters
    /// `line` the line to parse
    /// 
    /// # Return value
    /// `Ok(())` in case of success
    /// 
    /// `Err(error)` in case of error with `error` as the error code
    /// 
    /// # Panics
    /// Panics if line doesn't start with a `[` character, which indicates `line` is not a section declaration but may is a valid INI instruction. In this way, we can't return an error expecting a `[` at the beginning of the line, which doesn't make any sense
    fn parse_section(&mut self, line: &str) -> Result<(), Error> {
        let mut iter = line.char_indices();
        let leading_spaces = loop {
            match iter.next() {
                None => panic!("An INI section declaration starts with `[`. {} does not, which means the parser did not call the right function", line),
                Some((n, c)) => if c == '[' {
                    break n;
                } else if !c.is_whitespace() {
                    panic!("An INI section declaration starts with `[`. {} does not, which means the parser did not call the right function", line);
                },
            }
        };

        let mut end = 0;
        for (n, i) in iter.by_ref() {
            if i == ']' {
                end = n;
                break;
            }
        }

        // end == 0 means that there isn't any ']' while end == 1 means that the section name is empty
        if end == 0 {
            return Err(Error::from(ExpectedToken::new(String::from(line), leading_spaces, String::from("]"))));
        } else if end == 1 {
            return Err(Error::from(ExpectedIdentifier::new(String::from(line), leading_spaces + 1)));
        }

        let section = &line[leading_spaces + 1..end];
        if !Identifier::is_valid(section) {
            return Err(Error::from(InvalidIdentifier::new(String::from(line), String::from(section))));
        }

        // Checking integrity: I want to ensure there is no extra character after the section declaration
        // The only ones allowed are the whitespaces and the semicolon (with all the following ones)
        for (n, i) in iter {
            if i == ';' {
                break;
            } else if !i.is_whitespace() {
                let line = String::from(line);
                return Err(Error::from(UnexpectedToken::new(line, leading_spaces  // The leading spaces ignored
                                                                  + 2             // The '[' and ']' characters
                                                                  + section.len() // The identifier
                                                                  + n)));         // The index after the ']' character
            }
        }

        self.cur_section = Some(String::from(section));
        Ok(())
    }
}

/// Returns a subslice of the given slice which is comment-free (stopped at the first non-escaped semicolon ';'). `line` should be a single line
fn ignore_comment(line: &str) -> &str { 
    &line[..super::find_unescaped(line, ';').unwrap_or(line.len())]
}

/// Reads in an INI file and returns the parsed data
/// 
/// # Parameters
/// `path` the path of the file to open
/// 
/// # Return value
/// `Ok(data)` in case of success with `data` as a `HashMap<Identifier, Value>` linking each identifier to its associated value
/// 
/// `Err(error)` in case of failure with `error` as an error code for either an I/O error or a parsing error (see [ParseFileError](../errors/enum.ParseFileError.html "errors::ParseFileError"))
pub fn parse_file<T: AsRef<Path>>(path: T) -> Result<HashMap<Identifier, Value>, ParseFileError> {
    let mut file = File::open(path)?;

    let mut content = String::new();
    file.read_to_string(&mut content)?;
    let content = content;

    let mut parser = Parser::new();

    let mut begin = 0;
    while begin < content.len() {
        let end = match &content[begin..].find('\n') {
            Some(val) => val + begin,
            None      => content.len(),
        };
        if begin == end {
            begin += 1;
            continue;
        }

        let line = &content[begin..end];
        parser.parse_line(line)?;

        begin = end + 1;
    }

    Ok(parser.data())
}


#[cfg(test)]
mod tests;



================================================
File: mininip/src/parse/parser/tests.rs
================================================
use crate::parse::*;
use crate::datas::{Identifier, Value};
use crate::errors::Error;

#[test]
fn parser_parse_assignment_simplest() {
    let expr = "ident=val";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_commented() {
    let expr = "ident=val;This is a comment";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_with_spaces() {
    let expr = "ident = val";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_with_comment_and_spaces() {
    let expr = "ident=val ; This is a comment";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_with_leading_spaces() {
    let expr = "    ident=val";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_unicode_value() {
    let expr = r"latin_small_letter_e_with_acute=\x0000e9";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("latin_small_letter_e_with_acute"));
    let val = Value::Raw(String::from("\u{e9}"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_unicode_comment() {
    let expr = "ident=val; C'est un cas tout à fait valid"; // Notice the 'à' in the comment
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_str() {
    let expr = "ident='Hello world!'";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Str(String::from("Hello world!"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_int() {
    let expr = "ident=0";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Int(0);
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_float() {
    let expr = "ident=0.0";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Float(0.0);
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_bool() {
    let expr = "ident=on";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Bool(true);
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_assignment_unicode_identifier() {
    let expr = r"é=\x0000e9";
    let mut parser = Parser::new();

    match parser.parse_assignment(expr) {
        Ok(())                           => panic!("This code is wrong and shouldn't be accepted"),
        Err(Error::InvalidIdentifier(_)) => {},
        Err(err)                         => panic!("Wrong return value for this error: {:?}", err),
    }
}

#[test]
fn parser_parse_assignment_bad_ident() {
    let expr = "my*identifier=val";
    let mut parser = Parser::new();

    match parser.parse_assignment(expr) {
        Ok(())                           => panic!("This code is wrong and shouldn't be accepted"),
        Err(Error::InvalidIdentifier(_)) => {},
        Err(err)                         => panic!("Wrong return value for this error: {:?}", err),
    }
}

#[test]
fn parser_parse_assignment_bad_value() {
    let expr = "ident=abc=123";
    let mut parser = Parser::new();

    match parser.parse_assignment(expr) {
        Ok(())                        => panic!("This code is wrong and shouldn't be accepted"),
        Err(Error::ExpectedEscape(_)) => {},
        Err(err)                      => panic!("Wrong return value for this error: {:?}", err),
    }
}

#[test]
fn parser_parse_assignment_no_value() {
    let expr = "ident=";
    let mut parser = Parser::new();

    parser.parse_assignment(expr)
        .expect("This code should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::new());
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_section_simplest() {
    let expr = "[section]";
    let mut parser = Parser::new();

    parser.parse_section(expr)
        .expect("This code should be accepted because it's a valid INI section declaration");
    
    assert_eq!(parser.cur_section, Some(String::from("section")));

    parser.parse_assignment("ident=val").unwrap();

    let data = parser.data();
    let key = Identifier::new(Some(String::from("section")), String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_section_with_comment() {
    let expr = "[section];comment";
    let mut parser = Parser::new();

    parser.parse_section(expr)
        .expect("This code should be accepted because it's a valid INI section declaration");
    
    assert_eq!(parser.cur_section, Some(String::from("section")));

    parser.parse_assignment("ident=val").unwrap();

    let data = parser.data();
    let key = Identifier::new(Some(String::from("section")), String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_section_with_comment_and_whitespaces() {
    let expr = "[section]\t ; comment";
    let mut parser = Parser::new();

    parser.parse_section(expr)
        .expect("This code should be accepted because it's a valid INI section declaration");
    
    assert_eq!(parser.cur_section, Some(String::from("section")));

    parser.parse_assignment("ident=val").unwrap();

    let data = parser.data();
    let key = Identifier::new(Some(String::from("section")), String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_section_with_leading_spaces() {
    let expr = "    [section]";
    let mut parser = Parser::new();

    parser.parse_section(expr)
        .expect("This code should be accepted because it's a valid INI section declaration");

    assert_eq!(parser.cur_section, Some(String::from("section")));

    parser.parse_assignment("ident=val").unwrap();

    let data = parser.data();
    let key = Identifier::new(Some(String::from("section")), String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
#[should_panic]
fn parser_parse_section_leading_extra_token() {
    let expr = "char nullTerminatedString[BUFSIZ]";
    let mut parser = Parser::new();

    std::mem::drop(parser.parse_section(expr));
}

#[test]
fn parser_parse_section_ending_extra_token() {
    let expr = "[section] () -> bool { return true; }";
    let mut parser = Parser::new();

    match parser.parse_section(expr) {
        Ok(())                         => panic!("This code is wrong and shouldn't be accepted"),
        Err(Error::UnexpectedToken(_)) => {},
        Err(err)                       => panic!("Wrong return value: {:?}", err),
    }
}

#[test]
fn parser_parse_section_invalid_identifier() {
    let expr = "[hello there!]";
    let mut parser = Parser::new();

    match parser.parse_section(expr) {
        Ok(())                           => panic!("This code is wrong and shouldn't be accepted"),
        Err(Error::InvalidIdentifier(_)) => {},
        Err(err)                         => panic!("Wrong return value: {:?}", err),
    }
}

#[test]
fn parser_parse_section_empty() {
    let expr = "[]";
    let mut parser = Parser::new();

    match parser.parse_section(expr) {
        Ok(())                            => panic!("This code is wrong and shouldn't be accepted"),
        Err(Error::ExpectedIdentifier(_)) => {},
        Err(err)                          => panic!("Wrong return value: {:?}", err),
    }
}

#[test]
fn parser_parse_section_unterminated() {
    let expr = "[EOF";
    let mut parser = Parser::new();

    match parser.parse_section(expr) {
        Ok(())                       => panic!("This code is wrong and shouldn't be accepted"),
        Err(Error::ExpectedToken(_)) => {},
        Err(err)                     => panic!("Wrong return value: {:?}", err),
    }
}

#[test]
fn parser_parse_line_assignment() {
    let expr = "ident = val";
    let mut parser = Parser::new();

    parser.parse_line(expr)
        .expect("This line should be accepted because it's a valid INI assignment");

    let data = parser.data();
    let key = Identifier::new(None, String::from("ident"));
    let val = Value::Raw(String::from("val"));
    assert_eq!(data[&key], val);
}

#[test]
fn parser_parse_line_section() {
    let expr = "[section]";
    let mut parser = Parser::new();

    parser.parse_line(expr)
        .expect("This line should be accepted because it's a valid INI section declaration");

    assert_eq!(parser.cur_section, Some(String::from("section")));
}

#[test]
fn parser_parse_line_comment() {
    let expr = "; Just a comment";
    let mut parser = Parser::new();

    parser.parse_line(expr)
        .expect("This line should be accepted because it's a valid INI comment");
}

#[test]
fn parser_parse_line_empty() {
    let expr = "";
    let mut parser = Parser::new();

    parser.parse_line(expr)
        .expect("This line should be accepted because it's a valid INI empty line");
}



================================================
File: src/lib.rs
================================================
#![feature(abi_thiscall)]
#![feature(let_chains)]

use core::panic;
use std::{
    collections::{BTreeSet, HashMap},
    ffi::{c_void, OsStr},
    os::windows::prelude::OsStringExt,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicI32, AtomicU32, Ordering::Relaxed},
        Mutex,
    },
    time::{Duration, Instant, SystemTime},
};
mod netcode;
mod replay;
mod rollback;
mod sound;

use ilhook::x86::{HookPoint, HookType};

//use libloadng::Library;
#[cfg(feature = "logtofile")]
use log::info;
use mininip::datas::{Identifier, Value};
use netcode::{Netcoder, NetworkPacket};

//use notify::{RecursiveMode, Watcher};
use rollback::Rollbacker;
use sound::RollbackSoundManager;
use windows::{
    imp::{HeapAlloc, HeapFree, WaitForSingleObject},
    Win32::{
        Foundation::{HMODULE, HWND, TRUE},
        Networking::WinSock::closesocket,
        System::{
            Console::AllocConsole,
            Memory::{VirtualProtect, PAGE_PROTECTION_FLAGS, PAGE_READWRITE},
        },
    },
};

//mod netcode;
// +83E1
// +2836D
//004083dc actually
//00407a21 game thread created here

//#[cfg(debug_assertions)]
const ISDEBUG: bool = false;
//#[cfg(not(debug_assertions))]
//const ISDEBUG: bool = false;
#[cfg(feature = "logtofile")]
pub fn set_up_fern() -> Result<(), fern::InitError> {
    fern::Dispatch::new()
        // Perform allocation-free log formatting
        .format(|out, message, record| {
            out.finish(format_args!(
                "[{} {}] {}",
                //humantime::format_rfc3339(std::time::SystemTime::now()),
                record.level(),
                record.target(),
                message
            ))
        })
        // Add blanket level filter -
        .level(log::LevelFilter::Debug)
        .chain(fern::log_file("output.log")?)
        // Apply globally
        .apply()?;

    Ok(())
}

use winapi::um::libloaderapi::GetModuleFileNameW;

static mut ENABLE_PRINTLN: bool = false;

#[macro_export]
macro_rules! println {
    ($($arg:tt)*) => {{
        use crate::ENABLE_PRINTLN;
        #[allow(unused_unsafe)]
        if unsafe { ENABLE_PRINTLN } {
            std::println!($($arg)*);
        }
    }};
}

static HOOK: Mutex<Option<Box<[HookPoint]>>> = Mutex::new(None);

unsafe fn tamper_memory<T>(dst: *mut T, src: T) -> T {
    let mut old_prot_ptr: PAGE_PROTECTION_FLAGS = PAGE_PROTECTION_FLAGS(0);
    assert_eq!(
        VirtualProtect(
            dst as *const c_void,
            std::mem::size_of::<T>(),
            PAGE_READWRITE,
            std::ptr::addr_of_mut!(old_prot_ptr),
        ),
        TRUE,
    );
    let ori = dst.read_unaligned();
    dst.write_unaligned(src);
    VirtualProtect(
        dst as *const c_void,
        std::mem::size_of::<T>(),
        old_prot_ptr,
        std::ptr::addr_of_mut!(old_prot_ptr),
    );
    return ori;
}

// calling convention here was changed from cdecl to C because of the requirements of the new library. Thankfully they appear to be aliases in the current ABI
#[no_mangle]
pub unsafe extern "C" fn exeinit() {
    truer_exec(std::env::current_dir().unwrap());
}

//if I don't pass the path like this I get an "access violation". The library I'm using for the injection does mention that arguments must be Copy
static mut EXE_INIT_PATH: Vec<u8> = Vec::new();
#[no_mangle]
pub unsafe extern "C" fn better_exe_init_push_path(s: u8) {
    EXE_INIT_PATH.push(s);
}

#[no_mangle]
pub unsafe extern "C" fn better_exe_init() -> bool {
    let os = OsStr::from_encoded_bytes_unchecked(&EXE_INIT_PATH);

    truer_exec(PathBuf::from(os))
        .or_else(|| truer_exec(std::env::current_dir().unwrap()))
        .is_some()
}

#[no_mangle]
pub extern "C" fn Initialize(dllmodule: HMODULE) -> bool {
    let mut dat = [0u16; 1025];
    unsafe {
        GetModuleFileNameW(
            std::mem::transmute(dllmodule),
            &mut dat as *mut u16 as *mut u16,
            1024,
        )
    };

    let s = std::ffi::OsString::from_wide(&dat);

    //std::thread::sleep(Duration::from_millis(2000));
    //let m = init(0);
    let mut filepath = Path::new(&s).to_owned();
    filepath.pop();
    truer_exec(filepath);
    true
}
//687040 true real input buffer manipulation
// 85b8ec some related varible, 487040
#[no_mangle]
pub extern "cdecl" fn CheckVersion(a: *const [u8; 16]) -> bool {
    const HASH110A: [u8; 16] = [
        0xdf, 0x35, 0xd1, 0xfb, 0xc7, 0xb5, 0x83, 0x31, 0x7a, 0xda, 0xbe, 0x8c, 0xd9, 0xf5, 0x3b,
        0x2e,
    ];
    unsafe { *a == HASH110A }
}

static mut REAL_INPUT: Option<[bool; 10]> = None;
static mut REAL_INPUT2: Option<[bool; 10]> = None;

static mut UPDATE: Option<SystemTime> = None;
static mut TARGET: Option<u128> = None;

static TARGET_OFFSET: AtomicI32 = AtomicI32::new(0);
//static TARGET_OFFSET_COUNT: AtomicI32 = AtomicI32::new(0);

static mut TITLE: &'static [u16] = &[];
const VER: &str = env!("CARGO_PKG_VERSION");

unsafe extern "cdecl" fn skip(_a: *mut ilhook::x86::Registers, _b: usize, _c: usize) {}

//static SOUNDS_THAT_DID_HAPPEN: Mutex<BTreeMap<usize, Vec<usize>>> = Mutex::new(BTreeMap::new());

// set this mutex at the start of each frame. after each rollback you can see which sounds are left in this mutex. these sounds can and should be pasued
//static SOUND_THAT_MAYBE_HAPPEN: Mutex<BTreeMap<usize, Vec<usize>>> = Mutex::new(BTreeMap::new());
static mut SOUND_MANAGER: Option<RollbackSoundManager> = None;

static mut FORCE_SOUND_SKIP: bool = false;
//this is getting bad, fix the redundancy
//static INPUTS_RAW: Mutex<BTreeMap<usize, [u16; 2]>> = Mutex::new(BTreeMap::new());

static mut SPIN_TIME_MICROSECOND: i128 = 0;

static mut F62_ENABLED: bool = false;

const VERSION_BYTE_60: u8 = 0x6b;
const VERSION_BYTE_62: u8 = 0x6c;

static mut LAST_GAME_REQUEST: Option<[u8; 400]> = None;
static mut LAST_LOAD_ACK: Option<[u8; 400]> = None;
static mut LAST_MATCH_ACK: Option<[u8; 400]> = None;
static mut LAST_MATCH_LOAD: Option<[u8; 400]> = None;

pub fn force_sound_skip(soundid: usize) {
    unsafe {
        let forcesound = std::mem::transmute::<usize, extern "stdcall" fn(u32)>(0x401d50);
        FORCE_SOUND_SKIP = true;

        forcesound(soundid as u32);

        FORCE_SOUND_SKIP = false;
    }
}

//returns None on .ini errors
fn truer_exec(filename: PathBuf) -> Option<()> {
    #[cfg(feature = "allocconsole")]
    unsafe {
        AllocConsole();
    }

    let mut filepath = filename;
    filepath.push("giuroll.ini");
    //println!("{:?}", filepath);

    let conf = mininip::parse::parse_file(filepath).ok()?;

    #[cfg(feature = "logtofile")]
    {
        set_up_fern().unwrap();
        info!("here");
        std::panic::set_hook(Box::new(|x| info!("panic! {:?}", x)));
        let _ = set_up_fern();
    }

    unsafe {
        let (s, r) = std::sync::mpsc::channel();
        DATA_RECEIVER = Some(r);
        DATA_SENDER = Some(s);

        let (s, r) = std::sync::mpsc::channel();
        MEMORY_RECEIVER_FREE = Some(r);
        MEMORY_SENDER_FREE = Some(s);

        let (s, r) = std::sync::mpsc::channel();
        MEMORY_RECEIVER_ALLOC = Some(r);
        MEMORY_SENDER_ALLOC = Some(s);
    }

    fn read_ini_bool(
        conf: &HashMap<Identifier, Value>,
        section: &str,
        key: &str,
        default: bool,
    ) -> bool {
        conf.get(&Identifier::new(Some(section.to_string()), key.to_string()))
            .map(|x| match x {
                Value::Bool(x) => *x,
                _ => todo!("non bool .ini entry"),
            })
            .unwrap_or(default)
    }

    fn read_ini_int_hex(
        conf: &HashMap<Identifier, Value>,
        section: &str,
        key: &str,
        default: i64,
    ) -> i64 {
        conf.get(&Identifier::new(Some(section.to_string()), key.to_string()))
            .map(|x| match x {
                Value::Int(x) => *x,
                Value::Raw(x) | Value::Str(x) => {
                    i64::from_str_radix(x.strip_prefix("0x").unwrap(), 16).unwrap()
                }
                _ => todo!("non integer .ini entry"),
            })
            .unwrap_or(default)
    }

    fn read_ini_string(
        conf: &HashMap<Identifier, Value>,
        section: &str,
        key: &str,
        default: String,
    ) -> String {
        conf.get(&Identifier::new(Some(section.to_string()), key.to_string()))
            .map(|x| match x {
                Value::Str(x) => x.clone(),
                _ => todo!("non string .ini entry"),
            })
            .unwrap_or(default)
    }

    let inc = read_ini_int_hex(&conf, "Keyboard", "increase_delay_key", 0);
    let dec = read_ini_int_hex(&conf, "Keyboard", "decrease_delay_key", 0);
    let net = read_ini_int_hex(&conf, "Keyboard", "toggle_network_stats", 0);
    let spin = read_ini_int_hex(&conf, "FramerateFix", "spin_amount", 1500);
    let f62_enabled = read_ini_bool(&conf, "FramerateFix", "enable_f62", cfg!(feature = "f62"));
    let network_menu = read_ini_bool(&conf, "Netplay", "enable_network_stats_by_default", false);
    let default_delay = read_ini_int_hex(&conf, "Netplay", "default_delay", 2).clamp(0, 9);
    let autodelay_enabled = read_ini_bool(&conf, "Netplay", "enable_auto_delay", true);
    let frame_one_freeze_mitigation =
        read_ini_bool(&conf, "Netplay", "frame_one_freeze_mitigation", false);
    let autodelay_rollback = read_ini_int_hex(&conf, "Netplay", "auto_delay_rollback", 0);
    let soku2_compat_mode = read_ini_bool(&conf, "Misc", "soku2_compatibility_mode", false);
    let enable_println = read_ini_bool(
        &conf,
        "Misc",
        "enable_println",
        cfg!(feature = "allocconsole") || ISDEBUG,
    );

    //soku2 compatibility. Mods should change character size data themselves using exported functions. This is a temporary solution until soku2 team can implement that functionality.
    unsafe {
        if soku2_compat_mode {
            const CHARSIZEDATA_A: [usize; 35] = [
                2236, 2220, 2208, 2244, 2216, 2284, 2196, 2220, 2260, 2200, 2232, 2200, 2200, 2216,
                2352, 2224, 2196, 2196, 2216, 2216, 0, 2208, 2236, 2232, 2196, 2196, 2216, 2216,
                2200, 2216, 2352, 2200, 2284, 2220, 2208,
            ];

            const CHARSIZEDATA_B: [usize; 35] = [
                940, 940, 940, 944, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940,
                940, 940, 940, 940, 0, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940,
                940, 940,
            ];

            CHARSIZEDATA = (0..35)
                .map(|i| (CHARSIZEDATA_A[i], CHARSIZEDATA_B[i]))
                .collect();
        } else {
            const CHARSIZEDATA_A: [usize; 20] = [
                2236, 2220, 2208, 2244, 2216, 2284, 2196, 2220, 2260, 2200, 2232, 2200, 2200, 2216,
                2352, 2224, 2196, 2196, 2216, 2216,
            ];

            const CHARSIZEDATA_B: [usize; 20] = [
                940, 940, 940, 944, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940, 940,
                940, 940, 940, 940,
            ];

            CHARSIZEDATA = (0..20)
                .map(|i| (CHARSIZEDATA_A[i], CHARSIZEDATA_B[i]))
                .collect();
        }
    }

    let verstr: String = if f62_enabled {
        format!("{}CN", VER)
    } else {
        format!("{}", VER)
    };
    let mut title = read_ini_string(
        &conf,
        "Misc",
        "game_title",
        format!("Soku with giuroll {} :YoumuSleep:", verstr),
    );
    title.push('\0');

    let verstr = format!("Giuroll {}", verstr);

    let title = title.replace('$', &verstr);

    let tleak = Box::leak(Box::new(title));

    unsafe {
        TITLE = Box::leak(tleak.encode_utf16().collect::<Box<_>>());
        F62_ENABLED = f62_enabled;
        SPIN_TIME_MICROSECOND = spin as i128;
        INCREASE_DELAY_KEY = inc as u8;
        DECREASE_DELAY_KEY = dec as u8;
        TOGGLE_STAT_KEY = net as u8;
        TOGGLE_STAT = network_menu;
        LAST_DELAY_VALUE = default_delay as usize;
        DEFAULT_DELAY_VALUE = default_delay as usize;
        AUTODELAY_ENABLED = autodelay_enabled;
        AUTODELAY_ROLLBACK = autodelay_rollback as i8;
        ENABLE_PRINTLN = enable_println;
    }

    unsafe {
        let mut b = PAGE_PROTECTION_FLAGS(0);
        VirtualProtect(
            0x858b80 as *const c_void,
            1,
            PAGE_PROTECTION_FLAGS(0x40),
            &mut b,
        );

        *(0x858b80 as *mut u8) = if F62_ENABLED {
            VERSION_BYTE_62
        } else {
            VERSION_BYTE_60
        };
    }

    //meiling d236 desync fix, original by PinkySmile, Slen, cc/delthas, Fear Nagae, PC_Volt
    unsafe {
        let mut previous = PAGE_PROTECTION_FLAGS(0);
        VirtualProtect(
            0x724316 as *const c_void,
            4,
            PAGE_PROTECTION_FLAGS(0x40),
            &mut previous,
        );
        *(0x724316 as *mut u8) = 0x66;
        *(0x724317 as *mut u8) = 0xB9;
        *(0x724318 as *mut u8) = 0x0F;
        *(0x724319 as *mut u8) = 0x00;
        VirtualProtect(0x724316 as *const c_void, 4, previous, &mut previous);
    }

    // 9 digit font fix, by ichirin
    unsafe {
        for a in [0x43DC7D, 0x882954] {
            let mut previous = PAGE_PROTECTION_FLAGS(0);
            VirtualProtect(
                a as *const c_void,
                1,
                PAGE_PROTECTION_FLAGS(0x40),
                &mut previous,
            );
            *(a as *mut u8) = 0x0A;

            VirtualProtect(a as *const c_void, 1, previous, &mut previous);
        }
    }

    let new =
        unsafe { ilhook::x86::Hooker::new(0x482701, HookType::JmpBack(main_hook), 0).hook(6) };
    std::mem::forget(new);

    //0x899d60 maybe sound manager?
    unsafe extern "cdecl" fn handle_sound_real(
        a: *mut ilhook::x86::Registers,
        _b: usize,
        _c: usize,
    ) -> usize {
        //let sw = REQUESTED_THREAD_ID.swap(0, Relaxed);

        (*a).ecx = 0x89f9f8;
        (*a).eax = *(((*a).esp + 4) as *const u32);
        let soundid = (*a).eax as usize;

        if !BATTLE_STARTED || soundid == 0 {
            return if soundid == 0 { 0x401db7 } else { 0x401d58 };
        }

        if let Some(manager) = SOUND_MANAGER.as_mut() && !FORCE_SOUND_SKIP{

            //println!(
            //    "trying to play sound {} at frame {} with rollback {}",
            //    soundid,
            //    *SOKU_FRAMECOUNT,
            //    manager.current_rollback.is_some()
            //);
            if manager.insert_sound(*SOKU_FRAMECOUNT, soundid) {
                //println!("sound {} accepted at frame {}", soundid, *SOKU_FRAMECOUNT);
                0x401d58
            } else {
                //println!("sound {} rejected at frame {} because it was already present", soundid, *SOKU_FRAMECOUNT);
                0x401db7
            }
        } else {
            0x401d58
        }
    }

    let new = unsafe {
        ilhook::x86::Hooker::new(
            0x401d50, // 0x482820, //0x482532, sokuroll <-
            HookType::JmpToRet(handle_sound_real),
            0,
        )
        .hook(6)
    };
    std::mem::forget(new);

    unsafe extern "cdecl" fn soundskiphook1(
        a: *mut ilhook::x86::Registers,
        _b: usize,
        _c: usize,
    ) -> usize {
        if FORCE_SOUND_SKIP {
            // force call it, the return to end of function

            // 0x401d81

            let eax = *(((*a).esi + 4) as *const u32);
            let ecx = *(eax as *const u32);
            let fun = *((ecx + 0x48) as *const u32);
            let true_fun = std::mem::transmute::<usize, extern "thiscall" fn(u32, u32 /* , u32*/)>(
                fun as usize,
            );

            true_fun(ecx, eax /*, *(((*a).esp + 0x8)  as *const u32)*/);

            0x401db6
        } else {
            //replicate the usual logic

            if ((*(((*a).esp + 8) as *const usize)) & 1) == 0 {
                0x401d8c
            } else {
                0x401d81
            }
        }
    }

    let new = unsafe {
        ilhook::x86::Hooker::new(
            0x401d7a, // 0x482820, //0x482532, sokuroll <-
            HookType::JmpToRet(soundskiphook1),
            0,
        )
        .hook(5)
    };
    std::mem::forget(new);

    unsafe extern "cdecl" fn on_exit(_: *mut ilhook::x86::Registers, _: usize) {
        println!("on exit");
        HAS_LOADED = false;

        GIRLS_ARE_TALKING = false;
        LAST_LOAD_ACK = None;
        LAST_GAME_REQUEST = None;
        LAST_MATCH_ACK = None;
        LAST_MATCH_LOAD = None;
        LIKELY_DESYNCED = false;

        REQUESTED_THREAD_ID.store(0, Relaxed);
        NEXT_DRAW_PING = None;

        *(0x8971C0 as *mut usize) = 0; // reset wether to prevent desyncs
        ESC = 0;
        ESC2.store(0, Relaxed);
        BATTLE_STARTED = false;
        DISABLE_SEND.store(0, Relaxed);
        LAST_STATE.store(0, Relaxed);

        //SOUNDS_THAT_DID_HAPPEN.lock().unwrap().clear();
        //SOUND_THAT_MAYBE_HAPPEN.lock().unwrap().clear();

        //INPUTS_RAW.lock().unwrap().clear();
        let _heap = unsafe { *(0x89b404 as *const isize) };

        // we should be removing allocations that happen during frames which were rolled back, but that somehow breaks it, possibly because of some null check initializations
        //let allocset = std::mem::replace(&mut *ALLOCMUTEX.lock().unwrap(), BTreeSet::new());
        //let freeset = std::mem::replace(&mut *FREEMUTEX.lock().unwrap(), BTreeSet::new());
        //
        //for a in allocset.difference(&freeset) {
        //    //    unsafe { HeapFree(heap, 0, *a as *const c_void) };
        //    println!("freed but not alloced: {}", a);
        //}

        if let Some(x) = NETCODER.take() {
            let r = x.receiver;
            while r.try_recv().is_ok() {}
            DATA_RECEIVER = Some(r);
        }

        // it cannot be used by any different thread now
        if let Some(x) = ROLLBACKER.take() {
            for a in x.guessed {
                a.prev_state.did_happen();
            }
        }

        clean_replay_statics();

        GIRLSTALKED = false;
        NEXT_DRAW_ROLLBACK = None;
        NEXT_DRAW_ENEMY_DELAY = None;
    }

    //no_ko_sound
    /*
    explanation:
    sometimes rollback falsely cancels the KO sound. I believe this is because it's triggered from two different sites, and one of them, 0x6dcc0c
    seems to be triggered from a destructor. ~~The object whose destructor is cleared up here is likely overriden, and sokuroll does not restore that particular reference, because
    it's usually not relevant to rollback~~. After some experimenting I cannot find a cause for why the sound is called from two callsites, but no matter which one
    I remove the issue persist. It is also possible that instead of incorrect rollback, the sound is called before the frame, which is highly unusual for a sound,
    but so is having 2 call sites, that's why I think that's the most likely explanation.
    Sokuroll likely had an exception for the KO sound since usually it would never roll back that particualar sound, but I couldn't find a reference to it
    in the decompiled code. Here we simply remove that sound from it's 2 separate, unrelated callsites, and call it once, from a callsite that makes more sense.
    that callsite (can be found by searching no_ko in this file) is triggered not on the first frame after a knockdown, but on the second one, which is how it seems to work
    in vanilla game
    */
    unsafe {
        for addr in [0x6d8288, 0x6dcc0c] {
            let mut previous = PAGE_PROTECTION_FLAGS(0);
            VirtualProtect(
                addr as *const c_void,
                1,
                PAGE_PROTECTION_FLAGS(0x40),
                &mut previous,
            );
            *(addr as *mut u8) = 0x80;
        }
    };

    let new = unsafe { ilhook::x86::Hooker::new(0x481960, HookType::JmpBack(on_exit), 0).hook(6) };
    std::mem::forget(new);

    unsafe extern "cdecl" fn onexitexit(a: *mut ilhook::x86::Registers, _b: usize, _c: usize) {
        let f = std::mem::transmute::<usize, extern "fastcall" fn(u32)>((*a).edx as usize);

        f((*a).ecx);
        let allocset = std::mem::replace(&mut *ALLOCMUTEX.lock().unwrap(), BTreeSet::new());
        let freeset = std::mem::replace(&mut *FREEMUTEX.lock().unwrap(), BTreeSet::new());

        for a in allocset.difference(&freeset) {
            //    unsafe { HeapFree(heap, 0, *a as *const c_void) };
            println!("alloced but not freed: {}", a);
        }
        return;
        let allocset = std::mem::replace(&mut *ALLOCMUTEX.lock().unwrap(), BTreeSet::new());
        let freeset = std::mem::replace(&mut *FREEMUTEX.lock().unwrap(), BTreeSet::new());

        for a in &freeset {
            //    unsafe { HeapFree(heap, 0, *a as *const c_void) };
            println!("since then freed: {}", a);
        }
    }

    let return_insert_addr = 0x48196f + 5;
    let new = unsafe {
        ilhook::x86::Hooker::new(
            0x48196f,
            HookType::JmpToAddr(return_insert_addr, 0, onexitexit),
            0,
        )
        .hook(5)
    };

    std::mem::forget(new);

    let funnyaddr = return_insert_addr;
    let mut b = PAGE_PROTECTION_FLAGS(0);
    unsafe {
        VirtualProtect(
            funnyaddr as *const c_void,
            1,
            PAGE_PROTECTION_FLAGS(0x40),
            &mut b as *mut PAGE_PROTECTION_FLAGS,
        );

        *(funnyaddr as *mut u8) = 0xc3;
    }

    unsafe extern "cdecl" fn spectator_skip(
        a: *mut ilhook::x86::Registers,
        _b: usize,
        _c: usize,
    ) -> usize {
        let framecount_cur = *(((*a).esi + 0x4c) as *const u32);
        let edi = (*a).edi;

        //println!("edi: {}, framecount: {}", edi, framecount_cur);
        let no_skip = edi + 16 < framecount_cur && BATTLE_STARTED;
        if no_skip {
            /*
            LAB_0042daa6                                    XREF[1]:     0042daa0(j)
            0042daa6 8b 5e 48        MOV        EBX,dword ptr [ESI + 0x48]
            0042daa9 8b 4e 4c        MOV        ECX,dword ptr [ESI + 0x4c]
            */

            (*a).ebx = *(((*a).esi + 0x48) as *const u32);
            (*a).ecx = framecount_cur;

            0x42daac
        } else {
            //println!("here 3");
            /*
            0042db1d 8b 5c 24 1c     MOV        EBX,dword ptr [ESP + local_10]
             */
            (*a).ebx = *(((*a).esp + 0x1c) as *const u32);
            0x42db21
        }
    }

    // changes the spectator logic to only send frame if there are at least 10 frames in the buffer. this prevent spectator from desyncing
    let new = unsafe {
        ilhook::x86::Hooker::new(0x42daa6, HookType::JmpToRet(spectator_skip), 0).hook(6)
    };
    std::mem::forget(new);

    unsafe extern "cdecl" fn drawnumbers(_a: *mut ilhook::x86::Registers, _b: usize) {
        if let Some(x) = NEXT_DRAW_PING {
            draw_num((320.0, 466.0), x);
        }
        if let Some(x) = NEXT_DRAW_ROLLBACK {
            draw_num((340.0, 466.0), x);
        }

        if let Some(x) = NEXT_DRAW_ENEMY_DELAY {
            draw_num((20.0, 466.0), x);
        }
    }

    let new =
        unsafe { ilhook::x86::Hooker::new(0x43e320, HookType::JmpBack(drawnumbers), 0).hook(7) };
    std::mem::forget(new);

    unsafe extern "cdecl" fn ongirlstalk(_a: *mut ilhook::x86::Registers, _b: usize) {
        GIRLSTALKED = true;
        BATTLE_STARTED = false;
    }

    let new =
        unsafe { ilhook::x86::Hooker::new(0x482960, HookType::JmpBack(ongirlstalk), 0).hook(5) };
    std::mem::forget(new);

    unsafe {
        tamper_memory(
            0x00857170 as *mut unsafe extern "stdcall" fn(_, _, _) -> _,
            heap_free_override,
        );
        tamper_memory(
            0x00857174 as *mut unsafe extern "stdcall" fn(_, _, _) -> _,
            heap_alloc_override,
        );
    }

    let s = 0x822499; //0x822465;
    let new =
        unsafe { ilhook::x86::Hooker::new(s, HookType::JmpBack(heap_alloc_esi_result), 0).hook(6) };
    std::mem::forget(new);

    /*
       for c in [0x82346f, 0x8233ee, 0x82f125] {
           let new = unsafe {
               ilhook::x86::Hooker::new(
                   c,
                   HookType::JmpBack(reallochook),
                   ilhook::x86::CallbackOption::None,
                   0,
               )
               .hook()
           }
           .unwrap();
           hook.push(new);
       }
    */

    //prevent A pause in replay mode

    let new = unsafe { ilhook::x86::Hooker::new(0x48267a, HookType::JmpBack(apause), 0).hook(8) };
    std::mem::forget(new);

    // 0x428358 calls function checking if there is a next frame in net object

    let new =
        unsafe { ilhook::x86::Hooker::new(0x41daea, HookType::JmpBack(readonlinedata), 0).hook(5) };
    std::mem::forget(new);

    /*
    407f43 is being set to 8 upon ESC. 407f43 likely stores desired screen 0x8a0040 and the comparison with DAT_008a0044 is where the state gets bugged.
    if it's possible to "flush" the state to "go back to character select", that would be ideal
    */

    /*

       unsafe extern "cdecl" fn set_eax_to_0(a: *mut ilhook::x86::Registers, _b: usize) {
           //let r = *(0x8a0040 as *const u32);
           //println!("esc: {}", r);

           //(*a).eax = *(0x8a0040 as *const u32);
       }
       let new =
           unsafe { ilhook::x86::Hooker::new(0x407f1b, HookType::JmpBack(set_eax_to_0), 0).hook(6) };
       std::mem::forget(new);
    */

    unsafe extern "cdecl" fn override_current_game_state(
        a: *mut ilhook::x86::Registers,
        _b: usize,
    ) {
        if ESC2.load(Relaxed) != 0 {
            ESC2.store(0, Relaxed);
            if !GIRLS_ARE_TALKING {
                //println!("esc from opponent detected");

                if is_p1() {
                    (*a).eax = 8;
                } else {
                    (*a).eax = 9;
                }
            }
        }
    }
    let new = unsafe {
        ilhook::x86::Hooker::new(0x407f48, HookType::JmpBack(override_current_game_state), 0)
            .hook(6)
    };
    std::mem::forget(new);

    unsafe extern "cdecl" fn handle_raw_input(
        a: *mut ilhook::x86::Registers,
        _b: usize,
        _c: usize,
    ) {
        //        0046c902 8b ae 6c        MOV        EBP,dword ptr [ESI + 0x76c]

        (*a).ebp = *(((*a).esi + 0x76c) as *const u32);
        let input_manager = (*a).ecx;

        let real_input = match std::mem::replace(&mut REAL_INPUT, REAL_INPUT2.take()) {
            Some(x) => x,
            None => {
                let f = std::mem::transmute::<usize, extern "fastcall" fn(u32)>(0x040a370);
                (f)(input_manager);
                return;
            }
        };

        {
            let td = &mut *((input_manager + 0x38) as *mut i32);
            let lr = &mut *((input_manager + 0x3c) as *mut i32);

            match (real_input[0], real_input[1]) {
                (false, true) => *lr = (*lr).max(0) + 1,
                (true, false) | (true, true) => *lr = (*lr).min(0) - 1,
                _ => *lr = 0,
            }

            match (real_input[2], real_input[3]) {
                (false, true) => *td = (*td).max(0) + 1,
                (true, false) | (true, true) => *td = (*td).min(0) - 1,
                _ => *td = 0,
            }
        }

        for a in 0..6 {
            let v = &mut *((input_manager + 0x40 + a * 4) as *mut u32);

            if real_input[a as usize + 4] {
                *v += 1;
            } else {
                *v = 0;
            }
        }

        let m = &mut *((input_manager + 0x62) as *mut u16);
        *m = 0;
        for a in 0..10 {
            if real_input[a] {
                *m += 1 << a;
            }
        }
    }

    // todo : rename

    //return: 0x42839a
    //online input loop,

    let new = unsafe {
        ilhook::x86::Hooker::new(0x428374, HookType::JmpToAddr(0x42837f /*- 5*/, 0, skip), 0)
            .hook(5)
    };
    std::mem::forget(new);

    unsafe extern "cdecl" fn skiponcehost(
        a: *mut ilhook::x86::Registers,
        _b: usize,
        _c: usize,
    ) -> usize {
        if ESC > 120 {
            //old mechanism
            0x428393
        } else {
            //let skip = DISABLE_SEND.load(Relaxed) != 0;
            //DISABLE_SEND.store(1, Relaxed);

            let skip = true;

            if skip {
                0x428360
            } else {
                (*a).ecx = *(((*a).edi + 0x8) as *const u32);
                (*a).eax = *(((*a).ecx) as *const u32);
                0x428335
            }
        }
    }
    unsafe extern "cdecl" fn esc_host(a: *mut ilhook::x86::Registers, _b: usize) {
        //println!("host has esced");
        send_packet_untagged(Box::new([0x6e, 0]))
    }

    let new = unsafe { ilhook::x86::Hooker::new(0x428394, HookType::JmpBack(esc_host), 0).hook(5) };
    std::mem::forget(new);

    //input 00428341
    /*
    00481980 hm
     */
    let new =
        unsafe { ilhook::x86::Hooker::new(0x428330, HookType::JmpToRet(skiponcehost), 0).hook(5) };
    std::mem::forget(new);

    unsafe extern "cdecl" fn skiponceclient(
        a: *mut ilhook::x86::Registers,
        _b: usize,
        _c: usize,
    ) -> usize {
        if ESC > 120 {
            //old mechanism
            0x4286c3
        } else {
            //let skip = DISABLE_SEND.load(Relaxed) != 0;
            //DISABLE_SEND.store(1, Relaxed);

            let skip = true;

            if skip {
                0x428630
            } else {
                (*a).ecx = *(((*a).edi + 0x8) as *const u32);
                (*a).eax = *(((*a).ecx) as *const u32);
                0x428605
            }
        }
    }

    unsafe extern "cdecl" fn esc_client(a: *mut ilhook::x86::Registers, _b: usize) {
        //println!("client has esced");
        send_packet_untagged(Box::new([0x6e, 0]))
    }

    let new =
        unsafe { ilhook::x86::Hooker::new(0x428664, HookType::JmpBack(esc_client), 0).hook(5) };
    std::mem::forget(new);

    let new =
        unsafe { ilhook::x86::Hooker::new(0x428681, HookType::JmpBack(esc_client), 0).hook(5) };
    std::mem::forget(new);

    //not sure why client has two "esc" spaces but I'm not going to question it

    let new = unsafe {
        ilhook::x86::Hooker::new(0x428600, HookType::JmpToRet(skiponceclient), 0).hook(5)
    };
    std::mem::forget(new);

    let new = unsafe {
        ilhook::x86::Hooker::new(0x428644, HookType::JmpToAddr(0x42864f, 0, skip), 0).hook(5)
    };
    std::mem::forget(new);

    unsafe extern "cdecl" fn timing_loop(a: *mut ilhook::x86::Registers, _b: usize, _c: usize) {
        //#[cfg(feature = "f62")]
        //const TARGET_FRAMETIME: i32 = 1_000_000 / 62;
        //#[cfg(not(feature = "f62"))]
        //const TARGET_FRAMETIME: i32 = 1_000_000 / 60;

        let target_frametime = if F62_ENABLED {
            1_000_000 / 62
        } else {
            1_000_000 / 60
        };

        let waithandle = (*a).esi; //should I even use this? :/
        let (m, target) = match UPDATE {
            Some(x) => (x, TARGET.as_mut().unwrap()),
            None => {
                let m = SystemTime::now();
                UPDATE = Some(m);
                TARGET = Some(0);
                (m, TARGET.as_mut().unwrap())
            }
        };
        //let c = TARGET_OFFSET_COUNT.fetch_add(1, Relaxed);
        //if c % 10 == 0 {
        //    TARGET_OFFSET.store(0, Relaxed);
        //}

        let s = TARGET_OFFSET.swap(0, Relaxed).clamp(-1000, 10000);
        //TARGET_OFFSET.fetch_add(s / 2, Relaxed);
        *target += (target_frametime + s) as u128;

        let cur = m.elapsed().unwrap().as_micros();

        let diff = (*target as i128 + 1000) - cur as i128 - SPIN_TIME_MICROSECOND;
        //if diff > 1_000_000 {
        //    panic!("big diff {diff}");
        //}

        //info!("frame diff micro diff: {}", diff);
        let ddiff = (diff / 1000) as i32;
        if ddiff < 0 {
            println!("frameskip");
            #[cfg(feature = "logtofile")]
            info!("frameskip {diff}");
            if ddiff > 2 {
                *target = cur + (target_frametime) as u128;
            } else {
            }
        } else {
            WaitForSingleObject(waithandle as isize, ddiff as u32);
            if SPIN_TIME_MICROSECOND != 0 {
                loop {
                    let r1 = m.elapsed().unwrap().as_micros();
                    if r1 >= *target {
                        break;
                    }
                }
            }
        };
    }

    let new = unsafe {
        ilhook::x86::Hooker::new(0x4192f0, HookType::JmpToAddr(0x4193d7, 0, timing_loop), 0).hook(6)
    };
    std::mem::forget(new);
    //hook.push(new);

    let new = unsafe {
        ilhook::x86::Hooker::new(
            0x46c900,
            HookType::JmpToAddr(0x46c908, 0, handle_raw_input),
            0,
        )
        .hook(8)
    };
    std::mem::forget(new);

    unsafe extern "cdecl" fn sniff_sent(a: *mut ilhook::x86::Registers, _b: usize) {
        let ptr = ((*a).edi + 0x1c) as *const u8;
        let buf = std::slice::from_raw_parts(ptr, 400);

        if (buf[0] == 13 || buf[0] == 14) && buf[1] == 4 {
            let mut m = [0; 400];
            for i in 0..buf.len() {
                m[i] = buf[i];
            }
            LAST_GAME_REQUEST = Some(m);
        }

        if (buf[0] == 13 || buf[0] == 14) && buf[1] == 2 {
            let mut m = [0; 400];
            for i in 0..buf.len() {
                m[i] = buf[i];
            }

            LAST_LOAD_ACK = Some(m);
        }

        if (buf[0] == 13 || buf[0] == 14) && buf[1] == 5 {
            let mut m = [0; 400];
            for i in 0..buf.len() {
                m[i] = buf[i];
            }

            LAST_MATCH_ACK = Some(m);
        }

        if (buf[0] == 13 || buf[0] == 14) && buf[1] == 1 {
            let mut m = [0; 400];
            for i in 0..buf.len() {
                m[i] = buf[i];
            }

            LAST_MATCH_LOAD = Some(m);
        }
    }

    if frame_one_freeze_mitigation {
        let new =
            unsafe { ilhook::x86::Hooker::new(0x4171b4, HookType::JmpBack(sniff_sent), 0).hook(5) };
        std::mem::forget(new);

        let new =
            unsafe { ilhook::x86::Hooker::new(0x4171c7, HookType::JmpBack(sniff_sent), 0).hook(5) };
        std::mem::forget(new);
    }

    // disable x in replay 0x4826b5
    if false {
        unsafe {
            let mut previous = PAGE_PROTECTION_FLAGS(0);
            VirtualProtect(
                0x4826b5 as *const c_void,
                6,
                PAGE_PROTECTION_FLAGS(0x40),
                &mut previous,
            );
            std::slice::from_raw_parts_mut(0x4826b5 as *mut u8, 6).copy_from_slice(&[0x90; 6])
        };
    }

    //*HOOK.lock().unwrap() = Some(hook.into_boxed_slice());

    unsafe {
        std::thread::spawn(|| {
            //wait to avoid being overwritten by th123e
            std::thread::sleep(Duration::from_millis(3000));

            let mut whatever = PAGE_PROTECTION_FLAGS(0);
            VirtualProtect(
                0x89ffbe as *const c_void,
                1,
                PAGE_PROTECTION_FLAGS(0x40),
                &mut whatever,
            );

            windows::Win32::UI::WindowsAndMessaging::SetWindowTextW(
                *(0x89ff90 as *const HWND),
                windows::core::PCWSTR::from_raw(TITLE.as_ptr()),
            )
        })
    };

    Some(())
}

#[no_mangle]
pub extern "cdecl" fn cleanup() {
    if ISDEBUG {
        #[cfg(feature = "logtofile")]
        info!("cleaning up the hook")
    };

    //for a in std::mem::replace(&mut *FRAMES.lock().unwrap(), Vec::new()) {
    //    a.did_happen();
    //}

    HOOK.lock()
        .unwrap()
        .take()
        .unwrap()
        .into_vec()
        .into_iter()
        .for_each(|x| unsafe { x.unhook() });
}

unsafe fn set_input_buffer(input: [bool; 10], input2: [bool; 10]) {
    REAL_INPUT = Some(input);
    REAL_INPUT2 = Some(input2);
}
//might not be neccesseary
static REQUESTED_THREAD_ID: AtomicU32 = AtomicU32::new(0);

static mut NEXT_DRAW_PING: Option<i32> = None;
static mut NEXT_DRAW_ROLLBACK: Option<i32> = None;
static mut NEXT_DRAW_ENEMY_DELAY: Option<i32> = None;

static mut _NEXT_DRAW_PACKET_LOSS: Option<i32> = None;
static mut _NEXT_DRAW_PACKET_DESYNC: Option<i32> = None;

const SOKU_FRAMECOUNT: *mut usize = 0x8985d8 as *mut usize;
use windows::Win32::System::Threading::GetCurrentThreadId;

static FREEMUTEX: Mutex<BTreeSet<usize>> = Mutex::new(BTreeSet::new());

unsafe extern "stdcall" fn heap_free_override(heap: isize, flags: u32, s: *const c_void) -> i32 {

    //if let Some(x) = MEMORYMUTEX.lock().unwrap().remove(&*s) {
    //    if x != *SOKU_FRAMECOUNT {
    //        println!("freeing memory allocated at frame: {}, current: {}", x, *SOKU_FRAMECOUNT)
    //    }
    //}

    //if GetCurrentThreadId() == REQUESTED_THREAD_ID.load(Relaxed) {
    if
    /* !matches!(*(0x8a0040 as *const u8), 0x5 | 0xe | 0xd) || */
    *(0x89b404 as *const isize) != heap
        || GetCurrentThreadId() != REQUESTED_THREAD_ID.load(Relaxed)
        || *SOKU_FRAMECOUNT == 0
    {
        return HeapFree(heap as isize, flags as u32, s);
    }

    unsafe {
        MEMORY_SENDER_FREE
            .as_ref()
            .unwrap()
            .clone()
            .send(s as usize)
            .unwrap()
    };

    return 1;

    //} else {
    //    let heap = unsafe { *(0x89b404 as *const isize) };
    //    unsafe { HeapFree(heap, 0, *s as *const c_void) };
    //}
    //info!("{}", *s);
    //let mut f = FRAMES.lock().unwrap();
    //
    //match f.last_mut() {
    //    Some(x) => {
    //        x.frees.push(*s);
    //        //unsafe { *s = 0 };
    //    }
    //    None => (), //ignore
    //}
    //
    //A_COUNT.store(A_COUNT.load(Relaxed) + 1, Relaxed);
}

static ALLOCMUTEX: Mutex<BTreeSet<usize>> = Mutex::new(BTreeSet::new());
//static MEMORYMUTEX: Mutex<BTreeMap<usize, usize>> = Mutex::new(BTreeMap::new());

fn store_alloc(u: usize) {
    unsafe {
        //ALLOCMUTEX.lock().unwrap().insert(u);
        //MEMORYMUTEX.lock().unwrap().insert(u, *SOKU_FRAMECOUNT);
        //return;

        MEMORY_SENDER_ALLOC
            .as_ref()
            .unwrap()
            .clone()
            .send(u)
            .unwrap();
    }
}

static mut LIKELY_DESYNCED: bool = false;

#[no_mangle]
pub extern "cdecl" fn is_likely_desynced() -> bool {
    unsafe { LIKELY_DESYNCED }
}

unsafe extern "stdcall" fn heap_alloc_override(heap: isize, flags: u32, s: usize) -> *mut c_void {
    let ret = HeapAlloc(heap, flags, s);

    if *(0x89b404 as *const usize) != heap as usize
        /*|| !matches!(*(0x8a0040 as *const u8), 0x5 | 0xe | 0xd)*/
        || *SOKU_FRAMECOUNT == 0 ||
        GetCurrentThreadId() != REQUESTED_THREAD_ID.load(Relaxed)
    {
        //println!("wrong heap alloc");
    } else {
        store_alloc(ret as usize);
    }
    return ret;
}

unsafe extern "cdecl" fn heap_alloc_esi_result(a: *mut ilhook::x86::Registers, _b: usize) {
    if GetCurrentThreadId() == REQUESTED_THREAD_ID.load(Relaxed)
        /* && matches!(*(0x8a0040 as *const u8), 0x5 | 0xe | 0xd) */
        && *SOKU_FRAMECOUNT != 0
    {
        store_alloc((*a).esi as usize);
    }
}

#[allow(unused)]
unsafe extern "cdecl" fn reallochook(a: *mut ilhook::x86::Registers, _b: usize) {}

use core::sync::atomic::AtomicU8;

use crate::{
    netcode::{send_packet, send_packet_untagged},
    replay::{apause, clean_replay_statics, handle_replay},
    rollback::CHARSIZEDATA,
};

static LAST_STATE: AtomicU8 = AtomicU8::new(0x6b);
static mut HAS_LOADED: bool = false;

pub fn is_p1() -> bool {
    let is_p1 = unsafe {
        let netmanager = *(0x8986a0 as *const usize);
        *(netmanager as *const usize) == 0x858cac
    };
    is_p1
}

unsafe extern "cdecl" fn readonlinedata(a: *mut ilhook::x86::Registers, _b: usize) {
    const P1_PACKETS: [u8; 400] = [
        13, 3, 1, 0, 0, 0, 5, 2, 0, 0, 0, 0, 12, 0, 103, 0, 103, 0, 103, 0, 103, 0, 104, 0, 104, 0,
        104, 0, 104, 0, 106, 0, 106, 0, 200, 0, 203, 0, 208, 0, 208, 0, 208, 0, 208, 0, 1, 15, 0,
        0, 0, 189, 3, 21, 23, 251, 48, 70, 108, 0, 0, 0, 0, 0, 0, 221, 143, 113, 190, 134, 199,
        125, 39, 12, 12, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,
    ];

    const P2_PACKETS: [u8; 400] = [
        14, 3, 0, 0, 0, 0, 5, 1, 0, 0, 20, 100, 0, 100, 0, 101, 0, 101, 0, 102, 0, 102, 0, 103, 0,
        103, 0, 200, 0, 200, 0, 200, 0, 200, 0, 201, 0, 201, 0, 201, 0, 201, 0, 203, 0, 203, 0,
        203, 0, 203, 0, 1, 15, 119, 144, 191, 37, 118, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ];

    //both packets here are the same, both are the 0th packet, this is maybe unneccesseary

    let esp = (*a).esp;

    let packet_pointer = esp + 0x70;
    let slic = std::slice::from_raw_parts_mut(packet_pointer as *mut u8, 400);
    let type1 = slic[0];
    let type2 = slic[1];

    let count = usize::from_le_bytes(slic[2..6].try_into().unwrap());
    let sceneid = slic[6];
    //   let somethingweird = slic[7];
    //   let input1 = slic[8];
    //   let input2 = slic[9];

    //println!("{} , {}", &slic[0], &slic[1]);

    if type1 == 0x6e {
        //opponent esc
        if BATTLE_STARTED {
            ESC2.store(1, Relaxed);

            if !is_p1() {
                slic.copy_from_slice(&P1_PACKETS)
            } else {
                slic.copy_from_slice(&P2_PACKETS)
            }
        }
    } else if type1 == 0x6c {
        let buf = [0x6d, 0x61];
        let sock = *(((*a).edi + 0x28) as *const u32);
        let to = (*a).esp + 0x44;

        windows::Win32::Networking::WinSock::sendto(
            std::mem::transmute::<u32, windows::Win32::Networking::WinSock::SOCKET>(sock),
            &buf,
            0,
            to as *const windows::Win32::Networking::WinSock::SOCKADDR,
            0x10,
        );

        (*a).eax = 0x400;
    } else if type1 > 0x6c && type1 <= 0x80 {
        (*a).eax = 0x400;
    }

    if type1 == 0x6b {
        let m = DISABLE_SEND.load(Relaxed);

        if BATTLE_STARTED {
            let z = NetworkPacket::decode(slic);
            DATA_SENDER
                .as_ref()
                .unwrap()
                .send((z, Instant::now()))
                .unwrap();
        }

        if m < 150 {
            DISABLE_SEND.store(m + 1, Relaxed);

            let is_p1 = unsafe {
                let netmanager = *(0x8986a0 as *const usize);
                *(netmanager as *const usize) == 0x858cac
            };
            //the packet you receive first frame, every round. We are making it manually, to prevent data loss from freezing the game
            if !is_p1 {
                slic.copy_from_slice(&P1_PACKETS)
            } else {
                slic.copy_from_slice(&P2_PACKETS)
            }
        } else {
            (*a).eax = 0x400;
        }
    }

    if (type1 == 14 || type1 == 13) && type2 == 3 && sceneid == 0x5 && false {
        let is_p1 = unsafe {
            let netmanager = *(0x8986a0 as *const usize);
            *(netmanager as *const usize) == 0x858cac
        };

        println!(
            "received {} {}, data: {:?} as {}",
            type1, type2, slic, is_p1
        );
    }

    if type1 == 14 || type1 == 13 {
        if type2 == 4 {
            HAS_LOADED = true;
            //println!("has loaded !");
        }

        if type2 == 8 {
            if let Some(gr) = LAST_GAME_REQUEST {
                send_packet_untagged(Box::new(gr));
            }
        }

        if type2 == 1 && false {
            if let Some(gr) = LAST_LOAD_ACK {
                send_packet_untagged(Box::new(gr));
                println!("successfully sent 2 :)");
            } else {
                if let Some(gr) = LAST_GAME_REQUEST {
                    send_packet_untagged(Box::new(gr));
                    println!("successfully sent 3 :)");
                }
                println!("possibly shouldn't be here 2?");
            }
        }
    }

    if (type1 == 14 || type1 == 13) && type2 == 1 {
        //opponent has esced (probably) exit, the 60 is to avoid stray packets causing exits

        //if sceneid == 3 {
        //    // starting battle, likely we can safely reset ESC
        //    ESC = 0;
        //}
        if BATTLE_STARTED {
            ESC += 1;

            if ESC == 10 {
                let is_p1 = unsafe {
                    let netmanager = *(0x8986a0 as *const usize);
                    *(netmanager as *const usize) == 0x858cac
                };
                //the packet you receive first frame, every round. We are making it manually, to prevent data loss from freezing the game
                if !is_p1 {
                    slic.copy_from_slice(&P1_PACKETS)
                } else {
                    slic.copy_from_slice(&P2_PACKETS)
                }
            }

            //if ESC == 20 {
            //    *(0x8a0040 as *mut u32) = 8
            //}

            if ESC > 250 {
                println!("here stuck state detected");
                slic[0] = 0xb;
                ESC = 0;
                send_packet_untagged(Box::new([0xb]));
                let netmanager = *(0x8986a0 as *const usize);
                let socket = netmanager + 0x3e4;

                closesocket(*(socket as *const windows::Win32::Networking::WinSock::SOCKET));
            }
        }
        //info!("received {} {} {}", type1, type2, sceneid);
    }

    if type1 == 5 {
        let is_spect = slic[25] == 0;

        //println!("slic26: {:?}", &);
        if is_spect {
            if F62_ENABLED {
                slic[1] = VERSION_BYTE_62;
            } else {
                slic[1] = VERSION_BYTE_60;
            }
        }
        //is_spect = slic[]
        //let gamever = slic[1..17];
    }
    //BATTLE_STARTED
}
// network round start, stores round number
//00858cb8 c0 5f 45 00     addr      FUN_00455fc0            [3]

static mut GIRLSTALKED: bool = false;
static DISABLE_SEND: AtomicU8 = AtomicU8::new(0);

//todo: improve rewind mechanism

fn input_to_accum(inp: &[bool; 10]) -> u16 {
    let mut inputaccum = 0u16;
    for a in 0..10 {
        if inp[a] {
            inputaccum += 0x1 << a;
        }
    }
    inputaccum
}

unsafe fn read_key_better(key: u8) -> bool {
    let raw_input_buffer = 0x8a01b8;

    *((raw_input_buffer + key as u32) as *const u8) != 0
}

unsafe fn read_current_input() -> [bool; 10] {
    let local_input_manager = 0x898938;
    let raw_input_buffer = 0x8a01b8;
    let mut input = [false; 10];

    let controller_id = *((local_input_manager + 0x4) as *const u8);
    //if 255, then keyboard, if 0, or maybe something else, then controller

    if controller_id == 255 {
        //no controllers, reading keyboard input
        for a in 0..10 {
            let key = (local_input_manager + 0x8 + a * 0x4) as *const u8;

            let key = *key as u32;

            let key = *((raw_input_buffer + key) as *const u8) != 0;
            input[a] = key;
        }
    } else {
        let get_controller =
            std::mem::transmute::<usize, extern "thiscall" fn(u32, u32) -> u32>(0x40dc60);
        let controler = get_controller(0x8a0198, controller_id as u32);

        if controler != 0 {
            let axis1 = *(controler as *const i32);
            let axis2 = *((controler + 4) as *const i32);

            input[2] = axis1 < -500;
            input[3] = axis1 > 500;

            input[0] = axis2 < -500;
            input[1] = axis2 > 500;

            for a in 0..6 {
                let key = *((local_input_manager + 0x18 + a * 0x4) as *const i32);

                if key > -1 {
                    input[a + 4] = *((key as u32 + 0x30 + controler) as *const u8) != 0;
                }
            }
        }
    }

    input
}

static mut ROLLBACKER: Option<Rollbacker> = None;
static mut NETCODER: Option<Netcoder> = None;

static mut DATA_SENDER: Option<std::sync::mpsc::Sender<(NetworkPacket, Instant)>> = None;
static mut DATA_RECEIVER: Option<std::sync::mpsc::Receiver<(NetworkPacket, Instant)>> = None;

static mut MEMORY_SENDER_FREE: Option<std::sync::mpsc::Sender<usize>> = None;
static mut MEMORY_RECEIVER_FREE: Option<std::sync::mpsc::Receiver<usize>> = None;

static mut MEMORY_SENDER_ALLOC: Option<std::sync::mpsc::Sender<usize>> = None;
static mut MEMORY_RECEIVER_ALLOC: Option<std::sync::mpsc::Receiver<usize>> = None;

// this value is offset by 1, because we start sending frames at frame 1, meaning that input for frame n + 1 is sent in packet n
static mut LAST_DELAY_VALUE: usize = 0;
static mut DEFAULT_DELAY_VALUE: usize = 0;

static mut AUTODELAY_ENABLED: bool = false;
static mut AUTODELAY_ROLLBACK: i8 = 0;

static mut LAST_DELAY_MANIP: u8 = 0; // 0 neither, 1 up, 2 down, 3 both

static mut BATTLE_STARTED: bool = false;

static mut ESC: u8 = 0;
static mut GIRLS_ARE_TALKING: bool = false;

static mut ESC2: AtomicU8 = AtomicU8::new(0);

static mut INCREASE_DELAY_KEY: u8 = 0;
static mut DECREASE_DELAY_KEY: u8 = 0;

static mut TOGGLE_STAT_KEY: u8 = 0;

static mut TOGGLE_STAT: bool = false;
static mut LAST_TOGGLE: bool = false;

fn draw_num(pos: (f32, f32), num: i32) {
    let drawfn: extern "thiscall" fn(
        ptr: *const c_void,
        number: i32,
        x: f32,
        y: f32,
        a1: i32,
        a2: u8,
    ) = unsafe { std::mem::transmute::<usize, _>(0x414940) };

    drawfn(0x882940 as *const c_void, num, pos.0, pos.1, 0, 0);
}

fn pause(battle_state: &mut u32, state_sub_count: &mut u32) {
    if *battle_state != 4 {
        LAST_STATE.store(*battle_state as u8, Relaxed);
        *state_sub_count = state_sub_count.wrapping_sub(1);
        *battle_state = 4;
    }
}
fn resume(battle_state: &mut u32) {
    // should be called every frame because fo the logic set in fn pause involving state_sub_count
    let last = LAST_STATE.load(Relaxed);
    if last != 0x6b && *battle_state == 4 {
        //4 check to not accidentally override a state set by the game *maybe*
        *battle_state = last as u32;
        LAST_STATE.store(0x6b, Relaxed)
    }
}
//        info!("GAMETYPE TRUE {}", *(0x89868c as *const usize));

unsafe fn handle_online(
    framecount: usize,
    battle_state: &mut u32,
    cur_speed: &mut u32,
    cur_speed_iter: &mut u32,
    state_sub_count: &mut u32,
) {
    if framecount == 0 && !BATTLE_STARTED {
        let round = *((*(0x8986a0 as *const usize) + 0x6c0) as *const u8);

        BATTLE_STARTED = true;
        SOUND_MANAGER = Some(RollbackSoundManager::new());
        let m = DATA_RECEIVER.take().unwrap();

        let rollbacker = Rollbacker::new();

        ROLLBACKER = Some(rollbacker);
        let mut netcoder = Netcoder::new(m);
        if round == 1 {
            netcoder.autodelay_enabled = if AUTODELAY_ENABLED {
                Some(AUTODELAY_ROLLBACK)
            } else {
                None
            };
            netcoder.delay = DEFAULT_DELAY_VALUE;
        } else {
            netcoder.delay = LAST_DELAY_VALUE;
        }
        netcoder.max_rollback = 6;
        netcoder.display_stats = TOGGLE_STAT;
        NETCODER = Some(netcoder);

        //return;
    }

    if *battle_state == 6 {
        GIRLS_ARE_TALKING = true;
    }

    let rollbacker = ROLLBACKER.as_mut().unwrap();
    let netcoder = NETCODER.as_mut().unwrap();

    resume(battle_state);

    let stat_toggle = read_key_better(TOGGLE_STAT_KEY);
    if stat_toggle && !LAST_TOGGLE {
        TOGGLE_STAT = !TOGGLE_STAT;
        netcoder.display_stats = TOGGLE_STAT;
    }

    LAST_TOGGLE = stat_toggle;

    if *cur_speed_iter == 0 {
        let k_up = read_key_better(INCREASE_DELAY_KEY);
        let k_down = read_key_better(DECREASE_DELAY_KEY);

        let last_up = LAST_DELAY_MANIP & 1 == 1;
        let last_down = LAST_DELAY_MANIP & 2 == 2;

        if !last_up && k_up {
            if netcoder.delay < 9 {
                netcoder.delay += 1;
            }
        }

        if !last_down && k_down {
            if netcoder.delay > 0 {
                netcoder.delay -= 1;
            }
        }
        LAST_DELAY_VALUE = netcoder.delay;
        LAST_DELAY_MANIP = k_up as u8 + k_down as u8 * 2;

        let input = read_current_input();
        let speed = netcoder.process_and_send(rollbacker, input);

        *cur_speed = speed;

        if speed == 0 {
            pause(battle_state, state_sub_count);
            return;
        }
    }

    if let None = rollbacker.step(*cur_speed_iter as usize) {
        pause(battle_state, state_sub_count);
        return;
    }
}

unsafe extern "cdecl" fn main_hook(a: *mut ilhook::x86::Registers, _b: usize) {
    #[cfg(feature = "logtofile")]
    std::panic::set_hook(Box::new(|x| info!("panic! {:?}", x)));
    //REQUESTED_THREAD_ID.store(0, Relaxed);

    let framecount = *SOKU_FRAMECOUNT;

    let state_sub_count: &mut u32;
    let battle_state: &mut u32;
    let cur_speed: &mut u32;
    let cur_speed_iter: &mut u32;
    {
        let w = (*a).esi;
        cur_speed = &mut (*a).ebx;
        cur_speed_iter = &mut (*a).edi;

        let m = (w + 4 * 0x22) as *mut u32; //battle phase

        battle_state = &mut *m;
        state_sub_count = &mut *((w + 4) as *mut u32);
    }
    let gametype_main = *(0x898688 as *const usize);
    let is_netplay = *(0x8986a0 as *const usize) != 0;

    match (gametype_main, is_netplay) {
        (2, false) => {
            if framecount > 5 {
                REQUESTED_THREAD_ID.store(GetCurrentThreadId(), Relaxed);
            }

            handle_replay(
                framecount,
                battle_state,
                cur_speed,
                cur_speed_iter,
                state_sub_count,
            )
        } //2 is replay
        (1, true) => {
            // 1 is netplay and v player
            // todo: detect v player
            if framecount > 5 {
                REQUESTED_THREAD_ID.store(GetCurrentThreadId(), Relaxed);
            }

            if !GIRLSTALKED {
                handle_online(
                    framecount,
                    battle_state,
                    cur_speed,
                    cur_speed_iter,
                    state_sub_count,
                )
            }
        }
        _ => (),
    }

    if matches!(*battle_state, 3 | 5) {
        // together with the no_ko_sound hook. Explanation in the no_ko_sound hook.
        //IS_KO = true;
        if *state_sub_count == 1 {
            std::mem::transmute::<usize, extern "stdcall" fn(u32)>(0x439490)(0x2c);
        }
    } else {
        //IS_KO = false;
    }
}



================================================
File: src/netcode.rs
================================================
#[cfg(feature = "logtofile")]
use log::info;
use std::{
    collections::{HashMap, HashSet},
    sync::atomic::Ordering::Relaxed,
    time::{Duration, Instant},
};
use windows::Win32::Networking::WinSock::{sendto, SOCKADDR, SOCKET};

use crate::{
    input_to_accum, println, read_key_better, rollback::Rollbacker, LIKELY_DESYNCED, SOKU_FRAMECOUNT,
    TARGET_OFFSET,
};

#[derive(Clone, Debug)]
pub struct NetworkPacket {
    id: usize,
    desyncdetect: u8,

    delay: u8,
    max_rollback: u8,

    inputs: Vec<u16>, //also u8 in size? starts out at id + delay
    //confirms: Vec<bool>,
    last_confirm: usize,
    sync: Option<i32>,
}

impl NetworkPacket {
    fn encode(&self) -> Box<[u8]> {
        let mut buf = [0; 400];
        buf[4..8].copy_from_slice(&self.id.to_le_bytes()); //0
        buf[8] = self.desyncdetect;
        buf[9] = self.delay;
        buf[10] = self.max_rollback;

        buf[11] = self.inputs.len() as u8; //inputs, confirms are the same length

        for a in 0..self.inputs.len() {
            buf[(12 + a * 2)..(14 + a * 2)].copy_from_slice(&self.inputs[a].to_le_bytes());
        }

        let next = 12 + self.inputs.len() * 2;

        buf[next..next + 4].copy_from_slice(&self.last_confirm.to_le_bytes());
        let next = next + 4;

        buf[next..next + 4].copy_from_slice(&self.sync.unwrap_or(i32::MAX).to_le_bytes());
        let last = next + 4;

        buf[0..last].to_vec().into_boxed_slice()
    }

    pub fn decode(d: &[u8]) -> Self {
        let id = usize::from_le_bytes(d[4..8].try_into().unwrap());
        let desyncdetect = d[8];
        let delay = d[9];
        let max_rollback = d[10];
        let inputsize = d[11];
        let inputs = (0..inputsize as usize)
            .map(|x| u16::from_le_bytes(d[12 + x * 2..12 + (x + 1) * 2].try_into().unwrap()))
            .collect();
        let lastend = 12 + inputsize as usize * 2;
        let last_confirm = usize::from_le_bytes(d[lastend..lastend + 4].try_into().unwrap());

        let lastend = lastend + 4 as usize;
        let syncraw = i32::from_le_bytes(d[lastend..lastend + 4].try_into().unwrap());

        let sync = match syncraw {
            i32::MAX => None,
            x => Some(x),
        };

        Self {
            id,
            desyncdetect,
            delay,
            max_rollback,
            inputs,
            last_confirm,
            sync,
        }
    }
}

#[derive(Clone, Debug)]
pub enum FrameTimeData {
    Empty,
    LocalFirst(Instant),
    RemoteFirst(Instant),
    Done(i32),
}

pub struct Netcoder {
    last_opponent_confirm: usize,

    id: usize,

    //ideally we shouldn't be keeping a separate input stack from the Rollbacker but for now it's what I have
    opponent_inputs: Vec<Option<u16>>,
    last_opponent_input: usize,

    inputs: Vec<u16>,

    send_times: HashMap<usize, Instant>,
    recv_delays: HashMap<usize, Duration>,

    pub delay: usize,
    pub max_rollback: usize,
    pub display_stats: bool,
    pub last_opponent_delay: usize,

    past_frame_starts: Vec<FrameTimeData>,

    pub receiver: std::sync::mpsc::Receiver<(NetworkPacket, Instant)>,
    time_syncs: Vec<i32>,
    last_median_sync: i32,

    pub autodelay_enabled: Option<i8>,
}

/// The packets are only sent once per frame; a packet contains all previous unconfirmed inputs; a lost "main" packet is not recovered whenever it's not neccesseary
impl Netcoder {
    pub fn new(receiver: std::sync::mpsc::Receiver<(NetworkPacket, Instant)>) -> Self {
        Self {
            last_opponent_confirm: 0,
            inputs: Vec::new(),

            opponent_inputs: Vec::new(),

            send_times: HashMap::new(),
            recv_delays: HashMap::new(),

            last_opponent_delay: 0,
            last_opponent_input: 0,
            id: 0,
            delay: 0,
            max_rollback: 0,
            display_stats: false,

            past_frame_starts: Vec::new(),
            receiver,

            time_syncs: vec![],
            last_median_sync: 0,
            autodelay_enabled: None,
        }
    }

    /// returns whether or not we are allowed to proceed based on the confirmations we received
    /// and sends the following frame to the opponent
    pub fn process_and_send(
        &mut self,
        rollbacker: &mut Rollbacker,
        current_input: [bool; 10],
    ) -> u32 {
        let function_start_time = Instant::now();

        // self.id is lower than real framecount by 1, this is because we don't process frame 0
        while self.past_frame_starts.len() <= self.id {
            self.past_frame_starts.push(FrameTimeData::Empty);
        }

        let is_p1;
        unsafe {
            // todo: take out to it's own function
            let netmanager = *(0x8986a0 as *const usize);

            //host only
            let delay_display = (netmanager + 0x80) as *mut u8;
            *delay_display = self.delay as u8;

            //client only
            let delay_display = (netmanager + 0x81) as *mut u8;
            *delay_display = self.delay as u8;

            is_p1 = netmanager != 0 && *(netmanager as *const usize) == 0x858cac;
        }

        //because it looks like soku locks the netcode untill the start of a new frame, we sometimes reach this point before the netcode has finished processing it's packet, for that reason:
        std::thread::sleep(Duration::from_millis(1));

        while let Ok((packet, time)) = self.receiver.try_recv() {
            if packet.id > self.id + 20 {
                //these are probably packets comming from the last round, we better avoid them

                continue;
            }

            // time how long it took us to handlne that frame.
            // If we did not handle it in time we just send a -1000, meaning the opponent will slow down by a 1000 microseconds,
            // later on it should be worth to send information about frames ariving way too late,
            // that would make the opponent pause, or severely slow down for multiple frames

            //todo, handle time data packets not ariving at all, by taking the time of arrival of the subsequent packet

            if packet.id >= self.opponent_inputs.len() {
                if !is_p1 {
                    //self.delay = packet.delay as usize;
                    self.max_rollback = packet.max_rollback as usize;
                }

                if self.display_stats {
                    unsafe { crate::NEXT_DRAW_ENEMY_DELAY = Some(packet.delay as i32) };
                } else {
                    unsafe { crate::NEXT_DRAW_ENEMY_DELAY = None };
                }

                self.last_opponent_delay = packet.delay as usize;

                // is the first arrival of the newest packet
                let last = self
                    .past_frame_starts
                    .get(packet.id)
                    .cloned()
                    .unwrap_or(FrameTimeData::Empty);

                match last {
                    //bug! this value is set to -1000 even if we are less than 1000 microseconds from completing out frame, which is possible only for targets with
                    // less than 1000 microsecond ping. nevertheless it should be fixed at some point
                    FrameTimeData::Empty => {
                        //let r = if self.id + 1 < packet.id {
                        //    -((time.elapsed().as_micros()) as i128 / 100)
                        //} else {
                        //    -((time.elapsed().as_micros()) as i128 / 1000)
                        //};

                        while self.past_frame_starts.len() <= packet.id {
                            self.past_frame_starts.push(FrameTimeData::Empty);
                        }

                        self.past_frame_starts[packet.id] = FrameTimeData::RemoteFirst(time);
                        //Some(r)
                    }
                    FrameTimeData::LocalFirst(x) => {
                        let r = time
                            .checked_duration_since(x)
                            .unwrap_or_else(|| {
                                {
                                    {
                                        x.checked_duration_since(time)
                                            .expect("either of these opperation should succeed")
                                    }
                                }
                            })
                            .as_micros() as i128;
                        //info!("time passed: {}", r);

                        self.past_frame_starts[packet.id] = FrameTimeData::Done(r as i32);

                        //Some(r)
                    }

                    FrameTimeData::RemoteFirst(_) => {
                        //info!("same frame received twice");
                        ()
                    }
                    FrameTimeData::Done(_) => (),
                };

                //if let Some(my_diff) = my_diff {
                //    while self.past_frame_starts.len() <= packet.id {
                //        self.past_frame_starts.push(FrameTimeData::Empty);
                //    }
                //    self.past_frame_starts[packet.id] = FrameTimeData::Done(my_diff as i32);
                //}

                // handle opponents timing data
                if let Some(remote) = packet.sync {
                    //info!("frame diff {}", remote);
                    if remote < 0 {
                        TARGET_OFFSET.fetch_add(-remote.max(-5000), Relaxed);
                    } else {
                        match self
                            .past_frame_starts
                            .get(packet.id.saturating_sub((packet.inputs.len()) as usize))
                        {
                            Some(FrameTimeData::Done(local)) => {
                                let diff = *local - remote;

                                while packet.id > self.time_syncs.len() {
                                    self.time_syncs.push(0);
                                }
                                self.time_syncs.push(diff);

                                //TARGET_OFFSET.fetch_add(diff, Relaxed);
                            }
                            Some(FrameTimeData::RemoteFirst(_)) => {
                                //println!("frame diff: remote first");
                                TARGET_OFFSET.fetch_add(-200, Relaxed);
                            }
                            Some(_) => (),
                            None => (), //info!("no time packet"),
                        }
                    }
                    //info!("packet sync data: {:?}", x)
                }

                let weather_remote = packet.desyncdetect;
                let weather_local = rollbacker
                    .weathers
                    .get(&(packet.id.saturating_sub(20)))
                    .cloned()
                    .unwrap_or(0);
                if  weather_remote != weather_local {
                    //#[cfg(feature = "allocconsole")]
                    //println!("desync");
                    unsafe {
                        LIKELY_DESYNCED = true;
                    }
                    //todo, add different desync indication !
                    #[cfg(feature = "logtofile")]
                    info!(
                        "DESYNC: local: {}, remote: {}",
                        weather_local, weather_remote
                    )
                } else {
                    unsafe {
                        LIKELY_DESYNCED = false;
                    }
                }
            }

            let latest = packet.id as usize; //last delay
            while self.opponent_inputs.len() <= latest as usize {
                self.opponent_inputs.push(None);
            }
            let mut fr = latest;

            self.last_opponent_input = self.last_opponent_input.max(packet.id);

            for a in self.last_opponent_confirm..packet.last_confirm {
                let el = self.send_times.get(&a);
                if let Some(&x) = el {
                    let x = time.saturating_duration_since(x);
                    self.recv_delays.insert(fr, x);
                }
            }

            self.last_opponent_confirm = self.last_opponent_confirm.max(packet.last_confirm);

            for a in packet.inputs {
                if self.opponent_inputs[fr].is_none() {
                    //println!("{:?}", self.send_times[fr].elapsed());

                    let inp_a = a;

                    self.opponent_inputs[fr] = Some(inp_a);

                    // todo: move into it's own function

                    let inp = (0..10)
                        .into_iter()
                        .map(|x| (inp_a & (1 << x)) > 0)
                        .collect::<Vec<_>>()
                        .try_into()
                        .unwrap();
                    rollbacker.enemy_inputs.insert(inp, fr);
                }

                if fr == 0 {
                    break;
                }
                fr -= 1;
            }
        }

        let input_head = self.id;

        let input_range = self.last_opponent_confirm..=input_head;

        // do not override existing inputs; this can happen when delay is changed
        while rollbacker.self_inputs.len() <= input_head {
            rollbacker.self_inputs.push(current_input);
        }

        while self.inputs.len() <= input_head {
            self.inputs.push(input_to_accum(&current_input));
        }

        let mut ivec = self.inputs[input_range.clone()].to_vec();
        ivec.reverse();

        let past = match self.past_frame_starts.get(self.id.saturating_sub(30)) {
            Some(FrameTimeData::Done(x)) => Some(*x),
            _ => None,
        };

        let to_be_sent = NetworkPacket {
            id: self.id,
            desyncdetect: rollbacker
                .weathers
                .get(&(self.id.saturating_sub(20)))
                .cloned()
                .unwrap_or(0),
            delay: self.delay as u8,
            max_rollback: self.max_rollback as u8,
            inputs: ivec,
            last_confirm: (self.last_opponent_input).min(self.id + 30),
            sync: past,
        };

        unsafe { send_packet(to_be_sent.encode()) };
        self.send_times.insert(input_head, Instant::now());

        let m = rollbacker.start();

        let diff = self.id as i64 - unsafe { *SOKU_FRAMECOUNT } as i64;

        let m = if diff < (self.delay as i64) {
            m.saturating_sub(1)
        } else if diff > (self.delay as i64) {
            m + 1
        } else {
            m
        };

        //println!("m: {m}");

        //if rollbacker.guessed.len() > 13 {
        //    panic!("WHAT 13");
        //}

        unsafe {
            let m2 = rollbacker.guessed.len();

            if self.display_stats {
                if self.id % 60 == 0 && self.id > 0 {
                    let mut sum = 0;
                    for a in (self.id - 60)..(self.id) {
                        if let Some(x) = self.recv_delays.get(&a) {
                            sum += x.as_micros();
                        }
                    }
                    crate::NEXT_DRAW_PING = Some((sum / (60_000 * 2)) as i32);
                    crate::NEXT_DRAW_ROLLBACK = Some(m2 as i32);
                } else if crate::NEXT_DRAW_PING.is_none() {
                    crate::NEXT_DRAW_PING = Some(0);
                    crate::NEXT_DRAW_ROLLBACK = Some(m2 as i32);
                }
            } else {
                crate::NEXT_DRAW_PING = None;
                crate::NEXT_DRAW_ROLLBACK = None;
            }

            if let Some(bias) = self.autodelay_enabled {
                if self.id == 100 {
                    //let id = self.id - 60;
                    let iter = (30..70)
                        .map(|x| self.recv_delays.get(&x))
                        .filter_map(|x| x)
                        .map(|x| x.as_micros());

                    let (count, sum) = iter.fold((0, 0), |x, y| (x.0 + 1, x.1 + y));
                    let avg = sum / count;
                    println!("avg: {}", avg);
                    self.delay = ((avg / (1_000_000 / 30)) as i8 - bias).clamp(0, 9) as usize;
                }
            }
        }

        //time sync
        const TIME_SYNC_MEDIAN_INTERVAL: usize = 50;
        if self.id % TIME_SYNC_MEDIAN_INTERVAL == 0 && self.id > (TIME_SYNC_MEDIAN_INTERVAL + 30) {
            match self
                .time_syncs
                .get((self.id - 30 - TIME_SYNC_MEDIAN_INTERVAL)..(self.id - 30))
                .map(|x| {
                    let ret: Result<[i32; TIME_SYNC_MEDIAN_INTERVAL], _> = x.try_into();
                    ret.ok()
                })
                .flatten()
            {
                Some(mut av) => {
                    av.sort();

                    //let median = (av[TIME_SYNC_MEDIAN_INTERVAL / 2 - 1]
                    //    + av[TIME_SYNC_MEDIAN_INTERVAL / 2])
                    //    / 2;
                    //println!("median: {median}");
                    let sum: i32 = av[3..TIME_SYNC_MEDIAN_INTERVAL - 3].iter().sum();
                    let average = sum / (TIME_SYNC_MEDIAN_INTERVAL as i32 - 6);
                    println!("average: {average}");

                    self.last_median_sync = average;
                }
                None => (),
            }
        }
        if self.last_median_sync.abs() > 20000 {
            TARGET_OFFSET.fetch_add(self.last_median_sync / 700, Relaxed);
        } else if self.last_median_sync.abs() > 10000 {
            TARGET_OFFSET.fetch_add(self.last_median_sync / 1400, Relaxed);
        } else if self.last_median_sync.abs() > 2000 {
            TARGET_OFFSET.fetch_add(self.last_median_sync / 2000, Relaxed);
        } else {
            let res = if self.last_median_sync.abs() > 500 {
                self.last_median_sync.clamp(-1, 1)
            } else {
                0
            };
            TARGET_OFFSET.fetch_add(res, Relaxed);
        }

        if self.id > self.last_opponent_confirm + 30 {
            //crate::TARGET_OFFSET.fetch_add(1000 * m as i32, Relaxed);
            println!(
                "frame is missing: m: {m}, id: {}, confirm: {}",
                self.id, self.last_opponent_confirm
            );
            0
        } else if self.id
            > self.last_opponent_input
                + self.max_rollback
                + self.delay.max(self.last_opponent_delay)
        {
            //crate::TARGET_OFFSET.fetch_add(1000 * m as i32, Relaxed);
            println!(
                "frame is missing for reason 2: m: {m}, id: {}, confirm: {}",
                self.id, self.last_opponent_confirm
            );
            0
        } else {
            //no pause, perform additional operations here

            //todo: consider moving to it's own function
            match self.past_frame_starts[self.id].clone() {
                FrameTimeData::Empty => {
                    self.past_frame_starts[self.id] = FrameTimeData::LocalFirst(function_start_time)
                }
                FrameTimeData::LocalFirst(_) => todo!("should be unreachable"),
                FrameTimeData::RemoteFirst(x) => {
                    self.past_frame_starts[self.id] = FrameTimeData::Done(
                        x.saturating_duration_since(function_start_time).as_micros() as i32,
                    )
                }
                FrameTimeData::Done(_) => (),
            }

            self.id += 1;
            m as u32
        }
    }
}

pub unsafe fn send_packet(mut data: Box<[u8]>) {
    //info!("sending packet");
    data[0] = 0x6b;

    let netmanager = *(0x8986a0 as *const usize);

    let socket = netmanager + 0x3e4;

    let to;
    if *(netmanager as *const usize) == 0x858cac {
        let it = (netmanager + 0x4c8) as *const usize;
        data[1] = 1;

        if *it == 0 {
            panic!();
        }
        to = *(it as *const *const SOCKADDR);
    } else {
        data[1] = 2;

        if *(netmanager as *const usize) != 0x858d14 {
            panic!();
        }
        to = (netmanager + 0x47c) as *const SOCKADDR
    }

    // Some mods such as InfiniteDecks hook the import table of Soku
    let soku_sendto: unsafe extern "stdcall" fn(SOCKET, *const u8, i32, i32, *const SOCKADDR, i32) -> i32 =
    std::mem::transmute(0x0081f6c4);

    let rse = soku_sendto(*(socket as *const SOCKET), data.as_ptr(), data.len() as _, 0, to, 0x10);

    if rse == -1 {
        //to do, change error handling for sockets

        //#[cfg(feature = "logtofile")]
        //info!("socket err: {:?}", WSAGetLastError());
    }
}

pub unsafe fn send_packet_untagged(mut data: Box<[u8]>) {
    //info!("sending packet");

    let netmanager = *(0x8986a0 as *const usize);

    let socket = netmanager + 0x3e4;

    let to;
    if *(netmanager as *const usize) == 0x858cac {
        let it = (netmanager + 0x4c8) as *const usize;
        //data[1] = 1;

        if *it == 0 {
            panic!();
        }
        to = *(it as *const *const SOCKADDR);
    } else {
        //data[1] = 2;

        if *(netmanager as *const usize) != 0x858d14 {
            panic!();
        }
        to = (netmanager + 0x47c) as *const SOCKADDR
    }

    // Some mods such as InfiniteDecks hook the import table of Soku
    let soku_sendto: unsafe extern "stdcall" fn(SOCKET, *const u8, i32, i32, *const SOCKADDR, i32) -> i32 =
        std::mem::transmute(0x0081f6c4);

    let rse = soku_sendto(*(socket as *const SOCKET), data.as_ptr(), data.len() as _, 0, to, 0x10);

    if rse == -1 {
        //to do, change error handling for sockets

        //#[cfg(feature = "logtofile")]
        println!(
            "socket err: {:?}",
            windows::Win32::Networking::WinSock::WSAGetLastError()
        );
    }
}



================================================
File: src/replay.rs
================================================
use crate::{
    pause, read_key_better, println, resume,
    rollback::{dump_frame, Frame},
    MEMORY_RECEIVER_ALLOC, MEMORY_RECEIVER_FREE, SOKU_FRAMECOUNT,
};
use std::sync::{
    atomic::{AtomicU8, Ordering::Relaxed},
    Mutex,
};

static FRAMES: Mutex<Vec<Frame>> = Mutex::new(Vec::new());

static PAUSESTATE: AtomicU8 = AtomicU8::new(0);

static IS_REWINDING: AtomicU8 = AtomicU8::new(0);
static mut REWIND_PRESSED_LAST_FRAME: bool = false;

pub unsafe extern "cdecl" fn apause(_a: *mut ilhook::x86::Registers, _b: usize) {
    //let pinput = 0x89a248;
    //let input = read_addr(0x89a248, 0x58).usize_align();
    let pstate = PAUSESTATE.load(Relaxed);

    const ABUTTON: *mut usize = (0x89a248 + 0x40) as *mut usize;
    let a_input = *ABUTTON;
    *ABUTTON = 0;

    match (a_input, pstate) {
        (0, 1) => PAUSESTATE.store(2, Relaxed),
        (0, _) => (),
        (1, 0) => PAUSESTATE.store(1, Relaxed),
        (1, 1) => (),
        (1, 2) => PAUSESTATE.store(0, Relaxed),
        _ => (),
    }

    //if ISDEBUG { info!("input: {:?}", input[16]) };
}

pub unsafe fn clean_replay_statics() {
    for a in std::mem::replace(&mut *FRAMES.lock().unwrap(), vec![]) {
        a.did_happen();
    }
}

pub unsafe fn handle_replay(
    framecount: usize,
    battle_state: &mut u32,
    cur_speed: &mut u32,
    cur_speed_iter: &mut u32,
    weird_counter: &mut u32,
) {
    if let Some(x) = FRAMES.lock().unwrap().last_mut() {
        //TODO
        while let Ok(man) = MEMORY_RECEIVER_ALLOC.as_ref().unwrap().try_recv() {
            x.allocs.push(man);
        }

        while let Ok(man) = MEMORY_RECEIVER_FREE.as_ref().unwrap().try_recv() {
            x.frees.push(man);
            x.allocs.retain(|x| *x != man);
        }
    }
    let mut override_target_frame = None;

    resume(battle_state);
    if *cur_speed_iter == 0 && PAUSESTATE.load(Relaxed) != 0 && override_target_frame.is_none() {
        match (*cur_speed, REWIND_PRESSED_LAST_FRAME) {
            (16, false) => {
                REWIND_PRESSED_LAST_FRAME = true;
                override_target_frame = Some(framecount as u32 + 1);
            }
            (8, false) => {
                REWIND_PRESSED_LAST_FRAME = true;
                override_target_frame = Some(framecount as u32 - 2);
            }
            _ => {
                if *cur_speed == 1 {
                    REWIND_PRESSED_LAST_FRAME = false;
                }
                *cur_speed = 0;
                pause(battle_state, weird_counter);
                return;
            }
        }

        *cur_speed = 1;
    }

    if *cur_speed_iter == 0 {
        //"true" frame

        IS_REWINDING.store(0, Relaxed);
        let qdown = read_key_better(0x10);

        if qdown || override_target_frame.is_some() {
            let target = if let Some(x) = override_target_frame {
                x
            } else {
                let target = (framecount).saturating_sub(*cur_speed as usize + 1);
                target as u32
            };

            let mutex = FRAMES.lock().unwrap();
            let mut map = mutex;
            let frames = &mut *map;
            let mut last: Option<Frame> = None;
            loop {
                let candidate = frames.pop();
                if let Some(x) = candidate {
                    if let Some(x) = last {
                        x.never_happened();
                    }

                    let framenum = x.number as u32;
                    if framenum <= target {
                        IS_REWINDING.store(1, Relaxed);
                        //good
                        let diff = target - framenum;
                        
                        x.clone().restore();
                        //x.did_happen();
                        //dump_frame();
                        //unsafe {
                        //    FPST = x.fp;
                        //    asm!(
                        //        "FRSTOR {fpst}",
                        //        fpst = sym FPST
                        //    )
                        //}
                        //
                        //for a in x.adresses.clone().to_vec().into_iter() {
                        //    //if ISDEBUG { info!("trying to restore {}", a.pos) };
                        //    a.restore();
                        //    //if ISDEBUG { info!("success") };
                        //}

                        *cur_speed_iter = 1;
                        *cur_speed = 1 + diff;
                        //                        let diff = 1;

                        if diff <= 0 && false {
                            pause(battle_state, weird_counter);
                            *cur_speed_iter = *cur_speed;
                            return;
                        }

                        break;
                    } else {
                        last = Some(x);
                        continue;
                    }
                } else {
                    //nothing can be done ?

                    if let Some(last) = last {
                        last.restore();
                        //did it happen?
                    }
                    pause(battle_state, weird_counter);
                    *cur_speed_iter = *cur_speed;

                    return;
                }
            }
        }
    }

    let framecount = *SOKU_FRAMECOUNT;

    if framecount % 16 == 1 || IS_REWINDING.load(Relaxed) == 1 {
        
        let frame = dump_frame();

        let mut mutex = FRAMES.lock().unwrap();

        mutex.push(frame);
    }
}



================================================
File: src/rollback.rs
================================================
#[cfg(feature = "logtofile")]
use log::info;
use std::{
    arch::asm,
    collections::{BTreeSet, HashMap, HashSet},
    ffi::c_void,
};

use windows::{imp::HeapFree, Win32::System::Memory::HeapHandle};

use crate::{
    println, set_input_buffer, ISDEBUG, MEMORY_RECEIVER_ALLOC, MEMORY_RECEIVER_FREE, SOKU_FRAMECOUNT,
    SOUND_MANAGER,
};

type RInput = [bool; 10];

pub static mut CHARSIZEDATA: Vec<(usize, usize)> = vec![];

#[no_mangle]
pub unsafe extern "cdecl" fn set_char_data_size(s: usize) {
    while CHARSIZEDATA.len() > s {
        CHARSIZEDATA.pop();
    }

    while CHARSIZEDATA.len() < s {
        CHARSIZEDATA.push((0, 0))
    }
}

#[no_mangle]
pub unsafe extern "cdecl" fn set_char_data_pos(pos: usize, a: usize, b: usize) {
    CHARSIZEDATA[pos] = (a, b);
}

pub enum MemoryManip {
    Alloc(usize),
    Free(usize),
}
pub struct EnemyInputHolder {
    pub i: Vec<Option<RInput>>,
}

impl EnemyInputHolder {
    fn new() -> Self {
        Self { i: Vec::new() }
    }
    fn get(&self, count: usize) -> RInput {
        match self.get_result(count) {
            Ok(x) => x,
            Err(x) => x,
        }
    }

    pub fn insert(&mut self, input: RInput, frame: usize) {
        while frame >= self.i.len() {
            self.i.push(None);
        }
        if let Some(x) = self.i[frame].replace(input) {
            //doubled input
            if x != input {
                panic!("replacing existing input");
            }
        }
    }

    fn get_result(&self, frame: usize) -> Result<RInput, RInput> {
        match self.i.get(frame) {
            Some(Some(x)) => Ok(*x),
            None if frame == 0 => Err([false; 10]),
            Some(None) | None => {
                /*
                    in the future maybe try dropping inputs for attacks that are about to charge?
                    let mut w = (1..3)
                        .map(|x| self.get(frame.saturating_sub(x)))
                        .reduce(|x, y| {
                            (0..10)
                                .map(|idx| x[idx] & y[idx])
                                .collect::<Vec<_>>()
                                .try_into()
                                .unwrap()
                        })
                        .unwrap();

                    w[0..4].copy_from_slice(&self.get(frame - 1)[0..4]);
                */

                Err(self.get(frame - 1))
            }
        }
    }
}

pub struct Rollbacker {
    pub guessed: Vec<RollFrame>,

    current: usize,
    rolling_back: bool,

    pub future_sound: HashMap<usize, usize>,
    // first element is the sound, second is the frame it occured at, whenever a frame comes true we can delete all future sounds with that value

    // stores all the sounds that happened in "guessed" frames. Will also need to be topped up *after* last frame.
    // every frame we can store this as "past_sounds", and if any sound in future sounds did not appear in past_sounds, we can then cancell that sound by force calling 0x401d50
    // which we hook, and set a static to ignore the sound.

    //also, while rolling back, we should not play sounds that already did appear in past_sounds (and instead remove them, so we can see what is)
    pub enemy_inputs: EnemyInputHolder,
    pub self_inputs: Vec<RInput>,

    pub weathers: HashMap<usize, u8>,
}

impl Rollbacker {
    pub fn new() -> Self {
        Self {
            guessed: Vec::new(),
            current: 0,
            rolling_back: false,
            enemy_inputs: EnemyInputHolder::new(),
            self_inputs: Vec::new(),
            weathers: HashMap::new(),
            future_sound: HashMap::new(),
        }
    }

    /// fill in inputs before calling this function
    pub fn start(&mut self) -> usize {
        //this should only be called on the 0th iteration.
        self.current = unsafe { *SOKU_FRAMECOUNT };
        //let newsound = std::mem::replace(&mut *SOUNDS_THAT_DID_HAPPEN.lock().unwrap(), BTreeMap::new());

        while self.guessed.len() > 0
            && (self
                .enemy_inputs
                .get_result(self.guessed[0].prev_state.number)
                .map(|x| x == self.guessed[0].enemy_input)
                .unwrap_or(false))
        {
            let m = self.guessed.remove(0);

            self.weathers
                .insert(m.prev_state.number, m.prev_state.weather_sync_check);
            m.prev_state.did_happen();
            //let b = &mut *FREEMUTEX.lock().unwrap();
            //for a in m.prev_state.frees {
            //    b.insert(a);
            //}
        }

        //*SOUND_DELET_MUTEX.lock().unwrap() = newsound;

        self.rolling_back = false;
        self.guessed.len() + 1
    }

    fn apply_input(input: RInput, opponent_input: RInput) {
        let is_p1 = unsafe {
            let netmanager = *(0x8986a0 as *const usize);
            *(netmanager as *const usize) == 0x858cac
        };

        if is_p1 {
            unsafe { set_input_buffer(input, opponent_input) };
        } else {
            unsafe { set_input_buffer(opponent_input, input) };
        }
    }

    pub fn step(&mut self, iteration_number: usize) -> Option<()> {
        unsafe {
            if self.guessed.len() > 0 && (self.rolling_back || iteration_number == 0) {
                // this is how we were supposed to store the memory to be dealocated on-the-fly, but this also seems buggy
                let last = if iteration_number == 0 {
                    self.guessed.len() - 1
                } else {
                    iteration_number - 1
                };

                let pstate = &mut self.guessed[last].prev_state;
                while let Ok(man) = MEMORY_RECEIVER_FREE.as_ref().unwrap().try_recv() {
                    pstate.frees.push(man);
                }

                while let Ok(man) = MEMORY_RECEIVER_ALLOC.as_ref().unwrap().try_recv() {
                    //a
                    pstate.allocs.push(man);
                    //if !pstate.frees.contains(&man) {
                    //} else {
                    //    pstate.frees.retain(|x| *x != man);
                    //}
                }
            }
        }

        let tbr = if self.guessed.len() == iteration_number {
            //last iteration for this frame, handle sound here
            if self.rolling_back {
                unsafe {
                    let manager = SOUND_MANAGER.as_mut().unwrap();
                    manager.delete_non_matched();
                }
            }
            /*
            let mut to_be_skipped = vec![];
            {
                let new_sounds = &mut *SOUNDS_THAT_DID_HAPPEN.lock().unwrap();
                let old_sounds = &mut *SOUND_THAT_MAYBE_HAPPEN.lock().unwrap();

                let new_col = new_sounds
                    .values()
                    .map(|x| x.into_iter())
                    .flatten()
                    .collect::<HashSet<_>>();

                for idx in (self.current).saturating_sub(10)..=(self.current + 1) {
                    if new_sounds.contains_key(&(self.current + 1)) {
                        println!("HERE, CONTAINS")
                    }
                    if !new_sounds.contains_key(&idx) {
                        continue;
                    }
                    if let Some(x) = old_sounds.get(&idx) {
                        for a in x {
                            if !new_col.contains(a) {
                                to_be_skipped.push(*a);
                            }
                        }
                    }
                }

                std::mem::swap(old_sounds, new_sounds);
            };
            if to_be_skipped.len() != 0{
                println!("len: {}", to_be_skipped.len());
            }
            for a in to_be_skipped {
                force_sound_skip(a);
            }
            */

            //if self.current != unsafe { *SOKU_FRAMECOUNT } {
            //    println!("here");
            //}

            let current = unsafe {*SOKU_FRAMECOUNT};

            let si = self.self_inputs[current];
            let ei = self.enemy_inputs.get(current);
            Self::apply_input(si, ei);
            self.guessed.push(RollFrame::dump_with_guess(si, ei));

            Some(())
        } else {
            let fr = &mut self.guessed[iteration_number];

            if self.rolling_back {
                unsafe {
                    let frame = dump_frame();

                    let prev = std::mem::replace(&mut fr.prev_state, frame);
                    //prev.never_happened(); //this "variant" causes crashes
                    //let b = &mut *ALLOCMUTEX.lock().unwrap();
                    //for a in prev.allocs {
                    //    b.insert(a);
                    //}
                };
                fr.enemy_input = self.enemy_inputs.get(fr.prev_state.number);
                Self::apply_input(fr.player_input, fr.enemy_input);
                Some(())
            } else if fr.enemy_input != self.enemy_inputs.get(fr.prev_state.number) {
                //info!("ROLLBACK");
                unsafe {
                    let manager = SOUND_MANAGER.as_mut().unwrap();
                    manager.pop_sounds_since(fr.prev_state.number, self.current);
                }
                self.rolling_back = true;
                fr.prev_state.clone().restore();
                //fr.prev_state.clone().never_happened();
                fr.prev_state.frees.clear();
                fr.prev_state.allocs.clear();

                fr.enemy_input = self.enemy_inputs.get(fr.prev_state.number);
                Self::apply_input(fr.player_input, fr.enemy_input);
                Some(())
            } else {
                None
            }
        };

        tbr
    }
}

pub struct RollFrame {
    pub prev_state: Frame,
    pub player_input: RInput,
    pub enemy_input: RInput,
}

impl RollFrame {
    fn dump_with_guess(player_input: RInput, guess: RInput) -> Self {
        let prev_state = unsafe { dump_frame() };

        Self {
            prev_state,
            player_input: player_input,
            enemy_input: guess,
        }
    }
}
static mut FPST: [u8; 108] = [0u8; 108];
pub unsafe fn dump_frame() -> Frame {
    let w = unsafe {
        //let b = 3;
        asm!(
            "FSAVE {fpst}",
            "FRSTOR {fpst}",
            fpst = sym FPST
        );
        FPST
    };

    let mut m = vec![];

    #[cfg(feature = "logtofile")]
    if ISDEBUG {
        info!("0x895ec")
    };
    let ptr1 = read_addr(0x8985ec, 0x4);
    let first = get_ptr(&ptr1.content[0..4], 0);
    m.push(read_addr(first, 0xec));

    {
        let t = read_vec(first + 0x1c);

        m.push(t.read_underlying());

        m.push(t.to_addr());
    }

    {
        let t = read_vec(first + 0x68);
        if t.start != 0 {
            m.push(t.read_underlying());
        }
    }

    {
        let t = read_linked_list(first + 0x78);

        m.extend(t.read_all(0).to_vec().into_iter());
    }

    {
        let t = read_linked_list(first + 0xa4);

        m.extend(t.read_all(0x180).to_vec().into_iter());
    }

    {
        m.extend(
            read_maybe_ring_buffer(first + 0x28)
                .read_whole(0x10)
                .to_vec()
                .into_iter(),
        );
    }
    #[cfg(feature = "logtofile")]
    //0x8985e0
    if ISDEBUG {
        info!("0x8985e0")
    };
    let ptr1 = read_addr(0x8985e0, 0x4);
    let first = get_ptr(&ptr1.content[0..4], 0);
    m.push(read_addr(first, 0x118));

    m.extend(
        read_linked_list(first + 0x4)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );

    let llautosize = read_linked_list(first + 0x2c);
    m.push(llautosize.clone().to_addr());
    m.push(read_addr(first + 0x2c + 0xc, 4));

    let mut lit = llautosize.read_underlying().to_vec().into_iter();
    m.push(lit.next().unwrap().to_addr());

    for a in lit {
        let p = a.additional_data;
        if p != 0 {
            let size = match read_heap(p) {
                0 => 0x70, //very weird, but this is what sokuroll does
                x => x,
            };

            m.push(read_addr(p, size));
            m.push(a.clone().to_addr());
        }
    }

    m.extend(
        read_linked_list(first + 0x38)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );

    #[cfg(feature = "logtofile")]
    //0x8985f0
    if ISDEBUG {
        info!("0x8985f0")
    };
    //let ptr1 = read_addr(0x8985f0, 0x4);
    //let first = get_ptr(&ptr1.content[0..4], 0);

    let first = *(0x8985f0 as *const usize);

    m.push(read_addr(first, 0x94));

    m.push(read_vec(first + 0x10).read_underlying());

    m.push(read_vec(first + 0x20).read_underlying());
    #[cfg(feature = "logtofile")]
    if ISDEBUG {
        info!("0x8985f02")
    };
    m.extend(
        read_linked_list(first + 0x30)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );

    #[cfg(feature = "logtofile")]
    if ISDEBUG {
        info!("0x8985f03")
    };

    let effect_linked_list = read_linked_list(first + 0x5c).read_all(0x178).to_vec();

    m.extend(effect_linked_list.into_iter());

    #[cfg(feature = "logtofile")]
    //0x8985e8
    if ISDEBUG {
        info!("0x8985e8")
    };
    let read_weird_structure = |m: &mut Vec<_>, pos: usize, size: usize| {
        //I'm not quite sure what's going on here, or if it's infact correct
        let dat = read_addr(pos, 0x14);
        let n = dat.usize_align();

        let v1 = n[2];
        let v2 = n[3];
        let read_from = n[1];
        let v3 = n[4];

        if read_from == 0 {
            //println!("read_from is zero {:?}", n);
            if n[2] != 0 || n[3] != 0 || n[4] != 0 {
                #[cfg(feature = "logtofile")]
                if ISDEBUG {
                    info!("read_from is zero {:?}", n)
                };
            }
        } else {
            m.push(read_addr(read_from, v1 * 4));
        }
        for a in 0..v3 {
            let addr = *((read_from + ((a + v2) % v1) * 4) as *const usize);

            m.push(read_addr(addr, size));
        }
    };

    let ptr1 = read_addr(0x8985e8, 0x4);
    let first = get_ptr(&ptr1.content[0..4], 0);

    m.push(read_addr(first, 0x688));

    m.push(read_vec(first + 0x14).read_underlying());
    m.push(read_vec(first + 0x24).read_underlying());

    m.extend(
        read_linked_list(first + 0x34)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );

    m.extend(
        read_linked_list(first + 0x60)
            .read_all(0x178)
            .to_vec()
            .into_iter(),
    );

    read_weird_structure(&mut m, first + 0x18c, 0xc);
    read_weird_structure(&mut m, first + 0x1c0, 0xc);

    #[cfg(feature = "logtofile")]
    //0x8985e4
    if ISDEBUG {
        info!("0x8985e4")
    };

    let ptr1 = read_addr(0x8985e4, 0x4);
    let first = get_ptr(&ptr1.content[0..4], 0);
    m.push(read_addr(first, 0x908));
    m.extend(
        read_linked_list(first + 0x30)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );
    m.extend(
        read_linked_list(first + 0x3c)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );
    m.extend(
        read_linked_list(first + 0x48)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );
    m.extend(
        read_linked_list(first + 0x54)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );
    m.extend(
        read_linked_list(first + 0x60)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );
    m.extend(
        read_linked_list(first + 0x6c)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );

    {
        let w = read_vec(first + 0x9c);
        if w.start != 0 {
            m.push(w.read_underlying());
            #[cfg(feature = "logtofile")]
            info!("battle+x9c wasn't 0");
        }
        let w = read_vec(first + 0xac);

        if w.start != 0 {
            m.push(w.read_underlying());
            #[cfg(feature = "logtofile")]
            //seems to have never triggered, same as the one above
            info!("battle+xac wasn't 0");
        }
    }
    m.extend(
        read_linked_list(first + 0xbc)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );

    m.extend(
        read_linked_list(first + 0xe8)
            .read_all(0)
            .to_vec()
            .into_iter(),
    );

    //0x8985dc
    if ISDEBUG {
        #[cfg(feature = "logtofile")]
        info!("0x8985dc")
    };

    let ptr1 = read_addr(0x8985dc, 0x4);
    let first = get_ptr(&ptr1.content[0..4], 0);

    m.push(read_addr(first, 0x58));
    m.push(read_vec(first + 0x40).read_underlying());

    //0x8986a0
    #[cfg(feature = "logtofile")]
    if ISDEBUG {
        info!("0x8986a0")
    };

    //here sokuroll locks a mutex, but it seems unnecceseary

    let ptr1 = read_addr(0x8986a0, 0x4);
    let first = get_ptr(&ptr1.content[0..4], 0);
    // netplay input buffer. TODO: find corresponding input buffers in replay mode
    if first != 0 {
        m.push(read_addr(first + 0xf8, 0x68));
        m.push(read_addr(first + 0x174, 0x68));
    }

    #[cfg(feature = "logtofile")]
    if ISDEBUG {
        info!("0x8985e4")
    };

    let read_character_data = |p: usize, offset: usize, m: &mut Vec<_>| {
        let read_bullets = |pos: usize, char: u8, m: &mut Vec<_>| {
            let list = read_linked_list(pos);

            m.extend(list.clone().read_all(0).to_vec().into_iter());

            let und = list.read_underlying();

            for a in und.iter().skip(1) {
                m.push(a.clone().to_addr());
                let d = a.additional_data;
                if d != 0 {
                    let z = CHARSIZEDATA[char as usize % CHARSIZEDATA.len()].1;
                    let bullet = read_addr(d, z);
                    m.push(bullet.clone());
                    let p1 = get_ptr(&bullet.content, 0x3a4);

                    if p1 != 0 {
                        let ll = read_linked_list(d + 0x3a4);

                        m.extend(ll.read_all(0).to_vec().into_iter());
                    }

                    let p1 = get_ptr(&bullet.content, 0x17c);
                    if p1 != 0 {
                        let ll = read_linked_list(d + 0x17c);
                        m.extend(ll.read_all(0).to_vec().into_iter());
                    }

                    let p3 = get_ptr(&bullet.content, 0x35c);
                    if p3 != 0 {
                        let s = read_heap(p3);
                        if s > 4000 {
                            #[cfg(feature = "logtofile")]
                            {
                                info!("bullet data too big! {}", s)
                            }
                        } else {
                            m.push(read_addr(p3, s));
                        }
                    }

                    let p4 = get_ptr(&bullet.content, 0x354);
                    if p4 != 0 {
                        let nd = read_addr(p4, 0x54);
                        m.push(nd.clone());

                        let size = usize::from_le_bytes(nd.content[0x30..0x34].try_into().unwrap());
                        let ptr = usize::from_le_bytes(nd.content[0x2c..0x30].try_into().unwrap());

                        let n2 = read_addr(ptr, size * 4);
                        m.push(n2.clone());

                        for a in 0..size {
                            let p = get_ptr(&n2.content, a * 4);
                            if p != 0 {
                                m.push(read_addr(p, 0x10));
                            }
                        }

                        let size = usize::from_le_bytes(nd.content[0x44..0x48].try_into().unwrap());
                        let ptr = usize::from_le_bytes(nd.content[0x40..0x44].try_into().unwrap());

                        let n2 = read_addr(ptr, size * 4);
                        m.push(n2.clone());

                        for a in 0..size {
                            let p = get_ptr(&n2.content, a * 4);
                            if p != 0 {
                                m.push(read_addr(p, 0x10));
                            }
                        }

                        let size = usize::from_le_bytes(nd.content[0x8..0xc].try_into().unwrap())
                            * usize::from_le_bytes(nd.content[0x4..0x8].try_into().unwrap())
                            * 2
                            + 2;

                        let ptr = usize::from_le_bytes(nd.content[0x50..0x54].try_into().unwrap());

                        m.push(read_addr(ptr, size));
                    }
                }
            }
        };

        let old = *((p + 0xc + offset * 4) as *const usize);
        let char = old + 0x34c;
        let char = *(char as *const u8);

        let cdat = read_addr(old, CHARSIZEDATA[char as usize % CHARSIZEDATA.len()].0);
        m.push(cdat.clone());

        let bullets = old + 0x17c;
        read_bullets(bullets, char, m);

        if char == 5 {
            //youmu
            read_weird_structure(m, old + 0x8bc, 0x2c);
        }

        let mut z = vec![];
        let ll = read_linked_list(old + 0x718);

        z.push(read_addr(ll.ll4, 0xf4));

        for _ in 0..ll.listcount {
            let zcop = z.last().unwrap();
            let ptr = get_ptr(&zcop.content, 0);
            z.push(read_addr(ptr, 0xf4));
        }

        m.extend(z.into_iter());

        let new = get_ptr(&cdat.content, 0x6f8);
        m.push(read_addr(new, 0x68));

        let p4 = read_vec(new + 0x10);
        let w = p4.read_underlying();

        let i = p4.maybecapacity - p4.start;
        let i = (((i >> 0x1f) & 3) + i) >> 2;

        for a in 0..i {
            let p = get_ptr(&w.content, a * 4);

            if p != 0 {
                let o = read_addr(p, 4);
                let o2 = read_addr(p + 0x154, 4);

                m.push(o);
                m.push(o2);
            }
        }

        m.push(w.clone());

        m.push(p4.to_addr());

        let p5 = read_vec(new + 0x20);
        m.push(p5.read_underlying());
        m.push(p5.to_addr());

        let p6 = read_linked_list(new + 0x30);
        m.extend(p6.read_all(0).to_vec().into_iter());

        read_bullets(new + 0x5c, char, m);

        let p8 = read_maybe_ring_buffer(old + 0x7b0);
        m.extend(p8.read_whole(0x10).to_vec().into_iter());

        let p9 = read_maybe_ring_buffer(old + 0x5e8);
        m.extend(p9.read_whole(0x98).to_vec().into_iter());

        let p10 = read_maybe_ring_buffer(old + 0x5b0);
        m.extend(p10.read_whole(0x10).to_vec().into_iter());

        let p11 = read_maybe_ring_buffer(old + 0x5fc);
        m.extend(p11.read_whole(0x10).to_vec().into_iter());
    };

    let i3 = read_addr(0x8985e4, 4);

    let p3 = get_ptr(&i3.content, 0);

    read_character_data(p3, 0, &mut m);

    read_character_data(p3, 1, &mut m);

    #[cfg(feature = "logtofile")]
    if ISDEBUG {
        info!("bullets done");
    }

    m.push(read_addr(0x898718, 0x128));

    let sc1 = *(0x89881c as *const usize);
    // not sure what this is
    if sc1 != 0 {
        m.push(read_addr(sc1, 0x50));

        let sc2 = read_maybe_ring_buffer(sc1 + 0x3c);
        let z = sc2.obj_s as i32;

        #[cfg(feature = "logtofile")]
        if ISDEBUG {
            info!("weird deque done");
        }

        if z != 0 {
            let size = sc2.size as i32;
            let ptr = sc2.data as i32;

            let z = {
                let y = (sc2.f3 as i32 - 1 + z) % (size * 8);
                (ptr + ((y + (((y >> 0x1f) * 7) & 7)) >> 3)) as i32
            };

            let w = if ptr <= z - 0x50 { z - 0x50 } else { ptr };

            let x = (ptr + size).min(w + 0x28);

            m.push(read_addr(w as usize, (((x - w) >> 2) * 4) as usize));
        }
    }

    let to_be_read = vec![
        (0x898600, 0x6c),
        (0x8985d8, 4),
        (0x8985d4, 4),
        (0x8971b8, 0x20),
        (0x883cc8, 4),
        (0x89a88c, 4),
        (0x89a454, 4),
        (0x896d64, 8),
        (0x896b20, 4),
        (0x89b65c, 4),
        (0x89b660, 0x9c0),
        (0x89c01c, 4),
        (0x89aaf8, 4),
        (0x88526c, 4),
        (0x8971c0, 0x14),
        (0x8971c8, 0x4c),
    ];

    for (pos, size) in to_be_read {
        let x = read_addr(pos, size);

        m.push(x);
    }

    Frame {
        number: *SOKU_FRAMECOUNT,
        adresses: m.into_boxed_slice(),
        fp: w,
        frees: vec![],
        allocs: vec![],
        weather_sync_check: ((*(0x8971c4 as *const usize) * 16) + (*(0x8971c4 as *const usize) * 1)
            & 0xFF) as u8,
    }
}

fn read_heap(pos: usize) -> usize {
    unsafe {
        windows::Win32::System::Memory::HeapSize(
            *(0x89b404 as *const HeapHandle),
            windows::Win32::System::Memory::HEAP_FLAGS(0),
            pos as *const c_void,
        )
    }
}

#[derive(Debug, Clone)]
pub struct ReadAddr {
    pub pos: usize,
    pub content: Box<[u8]>,
}

impl ReadAddr {
    fn usize_align(&self) -> Box<[usize]> {
        self.content
            .chunks(4)
            .map(|x| usize::from_le_bytes(x.try_into().unwrap()))
            .collect()
    }

    pub fn restore(self) {
        if self.pos == 0 || self.content.len() == 0 {
            return;
        }
        let slice =
            unsafe { std::slice::from_raw_parts_mut(self.pos as *mut u8, self.content.len()) };
        slice.copy_from_slice(&self.content);
    }
}

#[derive(Debug, Clone)]
struct VecAddr {
    pub pos: usize,
    pub start: usize,
    pub maybecapacity: usize,
    pub end: usize,
}

#[derive(Debug, Clone)]
struct LL4 {
    pub pos: usize,
    pub next: usize,
    pub field2: usize,
    pub additional_data: usize,
}

impl LL4 {
    fn to_addr(self) -> ReadAddr {
        ReadAddr {
            pos: self.pos,
            content: [
                self.next.to_le_bytes(),
                self.field2.to_le_bytes(),
                self.additional_data.to_le_bytes(),
            ]
            .concat()
            .into_boxed_slice(),
        }
    }

    fn read_underlying_additional(&self, size: usize) -> ReadAddr {
        let ret = read_addr(self.additional_data, size);

        ret
    }
}

#[derive(Debug, Clone)]
struct LL3Holder {
    pub pos: usize,
    pub ll4: usize,
    pub listcount: usize,
    pub add_data: usize,
}

impl LL3Holder {
    fn read_underlying(&self) -> Box<[LL4]> {
        if self.ll4 == 0 {
            #[cfg(feature = "logtofile")]
            info!("ll4 is 0 ,painc");
            panic!("ll4 is 0");
        }

        let mut b = vec![read_ll4(self.ll4)];

        if self.listcount > 100000 {
            panic!("list too big");
        }

        for _ in 0..self.listcount {
            let last = b.last().unwrap();
            let next = last.next;
            if next == 0 {
                //#[cfg(feature = "logtofile")]
                //info!(
                //    "next was equal to zero, pos {}, out of {}",
                //    a,
                //    self.listcount - 1
                //);
                panic!();
            };
            b.push(read_ll4(next));
        }

        b.into_boxed_slice()
    }

    fn read_all(self, additional_size: usize) -> Box<[ReadAddr]> {
        //I think that readLL3 does not read itself, however, I will leave this here because it cannot hurt

        if self.listcount == 0 {
            Box::new([read_ll4(self.ll4).to_addr(), self.to_addr()])
        } else {
            let size = additional_size;
            if size == 0 {
                self.read_underlying()
                    .to_vec()
                    .into_iter()
                    .map(|x| x.to_addr())
                    .chain([self.to_addr()].into_iter())
                    .collect()
            } else {
                let mut uv = self.read_underlying().to_vec();
                let first = uv.remove(0);

                uv.into_iter()
                    .map(|x| {
                        if x.additional_data == 0 {
                            vec![x.to_addr()].into_iter()
                        } else {
                            let f = x.read_underlying_additional(size);
                            let sec = x.to_addr();
                            vec![f, sec].into_iter()
                        }
                    })
                    .flatten()
                    .chain([first.to_addr()].into_iter())
                    .chain([self.to_addr()].into_iter())
                    .collect()
            }
        }
    }

    fn to_addr(self) -> ReadAddr {
        ReadAddr {
            pos: self.pos,
            content: [
                self.ll4.to_le_bytes(),
                self.listcount.to_le_bytes(),
                self.add_data.to_le_bytes(),
            ]
            .concat()
            .into_boxed_slice(),
        }
    }
}

impl VecAddr {
    fn read_underlying(&self) -> ReadAddr {
        read_addr(self.start, self.end - self.start)
    }

    fn to_addr(self) -> ReadAddr {
        ReadAddr {
            pos: self.pos,
            content: [
                self.start.to_le_bytes(),
                self.maybecapacity.to_le_bytes(),
                self.end.to_le_bytes(),
            ]
            .concat()
            .into_boxed_slice(),
        }
    }
}
#[derive(Debug, Clone)]
struct Deque {
    #[allow(unused)]
    pos: usize,
    #[allow(unused)]
    f0: usize,
    data: usize,
    size: usize,
    f3: usize,
    obj_s: usize,
}

impl Deque {
    #[allow(unused)]
    fn to_addr(self) -> ReadAddr {
        ReadAddr {
            pos: self.pos,
            content: [
                self.f0.to_le_bytes(),
                self.data.to_le_bytes(),
                self.size.to_le_bytes(),
                self.f3.to_le_bytes(),
                self.obj_s.to_le_bytes(),
            ]
            .concat()
            .into_boxed_slice(),
        }
    }

    fn read_underlying(&self, size: usize) -> Box<[ReadAddr]> {
        let unknown: ReadAddr = read_addr(self.data, self.size * 4);

        unknown
            .content
            .clone()
            .chunks(4)
            .map(|x| usize::from_le_bytes(x.try_into().unwrap()))
            .filter(|x| *x != 0)
            .map(|x| read_addr(x, size))
            .chain([unknown].into_iter())
            .collect()
    }

    fn read_whole(self, size: usize) -> Box<[ReadAddr]> {
        if self.obj_s == 0 {
            return Box::new([]);
        }

        self.read_underlying(size)
    }
}

#[must_use]
fn read_addr(pos: usize, size: usize) -> ReadAddr {
    if size > 10000 {
        panic!("size too big {}", size);
    }
    if pos == 0 || size == 0 {
        #[cfg(feature = "logtofile")]
        if ISDEBUG {
            info!("unchecked 0 addr read")
        };
        return ReadAddr {
            pos: 0,
            content: Box::new([]),
        };
    }

    let ptr = pos as *const u8;
    ReadAddr {
        pos: pos,
        content: unsafe { std::slice::from_raw_parts(ptr, size) }.into(),
    }
}
#[must_use]
fn read_vec(pos: usize) -> VecAddr {
    let ptr = pos as *const u8;
    let content = unsafe { std::slice::from_raw_parts(ptr, 12) };

    VecAddr {
        pos: pos,
        start: usize::from_le_bytes(content[0..4].try_into().unwrap()),
        maybecapacity: usize::from_le_bytes(content[4..8].try_into().unwrap()),
        end: usize::from_le_bytes(content[8..12].try_into().unwrap()),
    }
}
#[must_use]
fn read_linked_list(pos: usize) -> LL3Holder {
    let w = read_addr(pos, 12);

    LL3Holder {
        pos: pos,

        ll4: usize::from_le_bytes(w.content[0..4].try_into().unwrap()),
        listcount: usize::from_le_bytes(w.content[4..8].try_into().unwrap()),
        add_data: usize::from_le_bytes(w.content[8..12].try_into().unwrap()),
    }
}
#[must_use]
fn read_ll4(pos: usize) -> LL4 {
    let w = read_addr(pos, 12);
    LL4 {
        pos: pos,
        next: usize::from_le_bytes(w.content[0..4].try_into().unwrap()),
        field2: usize::from_le_bytes(w.content[4..8].try_into().unwrap()),
        additional_data: usize::from_le_bytes(w.content[8..12].try_into().unwrap()),
    }
}

#[must_use]
fn read_maybe_ring_buffer(pos: usize) -> Deque {
    let m = read_addr(pos, 20);

    let w = m
        .content
        .chunks(4)
        .map(|x| usize::from_le_bytes(x.try_into().unwrap()))
        .collect::<Vec<_>>();

    Deque {
        pos: pos,
        f0: w[0],
        data: w[1],
        size: w[2],
        f3: w[3],
        obj_s: w[4],
    }
}

fn get_ptr(from: &[u8], offset: usize) -> usize {
    usize::from_le_bytes(from[offset..offset + 4].try_into().unwrap())
}

#[derive(Clone, Debug)]
pub struct Frame {
    pub number: usize,
    pub adresses: Box<[ReadAddr]>,
    pub fp: [u8; 108],
    pub frees: Vec<usize>,
    pub allocs: Vec<usize>,

    pub weather_sync_check: u8,
}

impl Frame {
    pub fn never_happened(self) {
        let allocs: BTreeSet<_> = self.allocs.iter().collect();
        let frees: BTreeSet<_> = self.frees.iter().collect();

        let heap = unsafe { *(0x89b404 as *const isize) };

        for a in allocs.intersection(&frees) {
            #[cfg(feature = "logtofile")]
            if ISDEBUG {
                info!("alloc: {}", a)
            };

            unsafe { HeapFree(heap, 0, (**a) as *const c_void) };
        }

        for a in allocs.difference(&frees) {
            //println!("never freed: {}", a);
        }
    }

    pub fn did_happen(self) {
        //let m = &mut *ALLOCMUTEX.lock().unwrap();
        //
        //for a in self.frees.iter() {
        //    m.remove(a);
        //}

        for a in self.frees {
            let heap = unsafe { *(0x89b404 as *const isize) };
            if a != 0 {
                unsafe { HeapFree(heap, 0, a as *const c_void) };
            }
        }
    }

    fn size_data(&self) -> String {
        let addr_total = self.adresses.iter().fold(0, |a, x| a + x.content.len());
        let freetotal = self.frees.len() * 4;
        let alloctotal = self.allocs.len() * 4;
        format!("addr: {addr_total} frees: {freetotal} allocs: {alloctotal}")
    }

    fn redundency_data(&self) -> String {
        let mut w = HashSet::new();
        let mut counter = 0;
        for a in self.adresses.iter() {
            for b in 0..a.content.len() {
                if !w.insert(a.pos + b) {
                    counter += 1;
                }
            }
        }

        format!("reduntant bytes: {}", counter)
    }

    pub fn restore(self) {
        unsafe {
            FPST = self.fp;
            asm!(
                "FRSTOR {fpst}",
                fpst = sym FPST
            )
        }

        for a in self.adresses.clone().to_vec().into_iter() {
            a.restore();
        }
    }
}

/*

unsafe fn deasm() {
    asm!(
        "PUSH       EBX",
        "PUSH       ESI",
        "MOV        ESI,param_1",
        "PUSH       EDI",
        "MOV        EBX,ESI",
        "CALL       FUN_100027d0",
        "MOV        EDI,0x6c",
        "MOV        ECX,0x898600",
        "CALL       readGameData",
        "MOV        EDI,0x4",
        "MOV        ECX=>Framecount2,0x8985d8",
        "CALL       readGameData",
        "MOV        ECX,0x8985d4",
        "CALL       readGameData",
        "MOV        EDI,0x20",
        "MOV        ECX,0x8971b8",
        "CALL       readGameData",
        "MOV        EDI,0x4",
        "MOV        ECX,0x883cc8",
        "CALL       readGameData",
        "MOV        ECX=>DAT_0089a88c,0x89a88c",
        "CALL       readGameData",
        "MOV        ECX,0x89a454",
        "CALL       readGameData",
        "MOV        EDI,0x8",
        "MOV        ECX,0x896d64",
        "CALL       readGameData",
        "MOV        EDI,0x4",
        "MOV        ECX=>DAT_00896b20,0x896b20",
        "CALL       readGameData",
        "MOV        ECX,0x89b65c",
        "CALL       readGameData",
        "MOV        EDI,0x9c0",
        "MOV        ECX,0x89b660",
        "CALL       readGameData",
        "MOV        EDI,0x4",
        "MOV        ECX,0x89c01c",
        "CALL       readGameData",
        "MOV        ECX,0x89aaf8",
        "CALL       readGameData",
        "MOV        ECX,0x88526c",
        "CALL       readGameData",
        "MOV        EDI,0x14",
        "MOV        ECX,0x8971c0",
        "CALL       readGameData",
        "MOV        ECX,dword ptr [DAT_008971c8]",
        "MOV        EDI,0x4c",
        "CALL       readGameData",
        "MOV        EBX,dword ptr [DAT_008985ec]",
        "MOV        EDI,0xec",
        "MOV        ECX,EBX",
        "CALL       readGameData",
        "LEA        param_1,[EBX + 0x1c]",
        "CALL       store_autosize",
        "LEA        param_1,[EBX + 0x68]",
        "CALL       store_autosize",
        "PUSH       0x0",
        "LEA        param_1,[EBX + 0x78]",
        "PUSH       param_1",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x180",
        "LEA        ECX,[EBX + 0xa4]",
        "PUSH       ECX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "LEA        param_1,[EBX + 0x28]",
        "MOV        ECX,ESI",
        "CALL       ReadWeirderLinkedList",
        "MOV        EBX,dword ptr [DAT_008985e0]",
        "MOV        EDI,0x118",
        "MOV        ECX,EBX",
        "CALL       readGameData",
        "PUSH       0x0",
        "LEA        EDX,[EBX + 0x4]",
        "PUSH       EDX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "LEA        param_1,[EBX + 0x2c]",
        "PUSH       param_1",
        "PUSH       ESI",
        "CALL       ReadSizeDetect",
        "ADD        ESP,0x8",
        "PUSH       0x0",
        "ADD        EBX,0x38",
        "PUSH       EBX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "MOV        EBX,dword ptr [DAT_008985f0]",
        "MOV        EDI,0x94",
        "MOV        ECX,EBX",
        "CALL       readGameData",
        "LEA        param_1,[EBX + 0x10]",
        "CALL       store_autosize",
        "LEA        param_1,[EBX + 0x20]",
        "CALL       store_autosize",
        "PUSH       0x0",
        "LEA        ECX,[EBX + 0x30]",
        "PUSH       ECX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x178",
        "ADD        EBX,0x5c",
        "PUSH       EBX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "MOV        EBX,dword ptr [DAT_008985e8]",
        "MOV        EDI,0x688",
        "MOV        ECX,EBX",
        "CALL       readGameData",
        "LEA        param_1,[EBX + 0x14]",
        "CALL       store_autosize",
        "LEA        param_1,[EBX + 0x24]",
        "CALL       store_autosize",
        "PUSH       0x0",
        "LEA        EDX,[EBX + 0x34]",
        "PUSH       EDX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x178",
        "LEA        param_1,[EBX + 0x60]",
        "PUSH       param_1",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "LEA        param_1,[EBX + 0x18c]",
        "MOV        ECX,ESI",
        "CALL       ReadLinkedListWrappingBig",
        "LEA        param_1,[EBX + 0x1c0]",
        "MOV        ECX,ESI",
        "CALL       ReadLinkedListWrappingBig",
        "MOV        EBX,dword ptr [DAT_008985e4]",
        "MOV        EDI,0x908",
        "MOV        ECX,EBX",
        "CALL       readGameData",
        "PUSH       0x0",
        "LEA        ECX,[EBX + 0x30]",
        "PUSH       ECX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x0",
        "LEA        EDX,[EBX + 0x3c]",
        "PUSH       EDX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x0",
        "LEA        param_1,[EBX + 0x48]",
        "PUSH       param_1",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x0",
        "LEA        ECX,[EBX + 0x54]",
        "PUSH       ECX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x0",
        "LEA        EDX,[EBX + 0x60]",
        "PUSH       EDX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x0",
        "LEA        param_1,[EBX + 0x6c]",
        "PUSH       param_1",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "LEA        param_1,[EBX + 0x9c]",
        "CALL       store_autosize",
        "LEA        param_1,[EBX + 0xac]",
        "CALL       store_autosize",
        "PUSH       0x0",
        "LEA        ECX,[EBX + 0xbc]",
        "PUSH       ECX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "PUSH       0x0",
        "ADD        EBX,0xe8",
        "PUSH       EBX",
        "MOV        param_1,ESI",
        "CALL       readLL3",
        "MOV        EBX,dword ptr [DAT_008985dc]",
        "MOV        EDI,0x58",
        "MOV        ECX,EBX",
        "CALL       readGameData",
        "LEA        param_1,[EBX + 0x40]",
        "CALL       store_autosize",
        "XOR        ECX,ECX",
        "MOV        param_1,ESI",
        "CALL       FUN_100180f0",
        "LEA        ECX,[EDI + -0x57]",
        "MOV        param_1,ESI",
        "CALL       FUN_100180f0",
        "MOV        EDX,dword ptr [Mutex1]",
        "MOV        EBX,dword ptr [DAT_008986a0]",
        "PUSH       -0x1",
        "LEA        ECX,[EBX + 0xf8]",
        "MOV        EDI,0x68",
        "CALL       readGameData",
        "LEA        ECX,[EBX + 0x174]",
        "CALL       readGameData",
        "MOV        EBX,dword ptr [DAT_0089881c]",
        "MOV        EDI,0x128",
        "MOV        ECX,0x898718",
        "CALL       readGameData",
        "MOV        EDI,0x50",
        "MOV        ECX,EBX",
        "CALL       readGameData",
        "LEA        param_1,[EBX + 0x3c]",
        "PUSH       ESI",
        "CALL       FUN_100026f0",
        "POP        EDI",
        "POP        ESI",
        "POP        EBX",
        "RET",
    )
}
 */



================================================
File: src/sound.rs
================================================
use std::collections::{HashMap, HashSet};

use crate::{force_sound_skip, println, SOKU_FRAMECOUNT};

pub struct RollbackSoundManager {
    sounds_that_did_happen: HashMap<usize, Vec<usize>>,
    sounds_that_maybe_happened: HashMap<usize, Vec<usize>>,
    pub current_rollback: Option<usize>,
}

impl RollbackSoundManager {
    pub fn new() -> Self {
        Self {
            sounds_that_did_happen: HashMap::new(),
            sounds_that_maybe_happened: HashMap::new(),
            current_rollback: None,
        }
    }

    pub fn insert_sound(&mut self, frame: usize, sound: usize) -> bool {
        //if let Some(x) = self.sounds_that_maybe_happened.get_mut(&frame) {
        //    let index = x.iter().position(|x| *x == sound);
        //    if let Some(idx) = index {
        //        //x.remove(idx);
        //        return false;
        //    }
        //}

        let s = self
            .sounds_that_did_happen
            .entry(frame)
            .or_insert_with(|| Vec::new());
        s.push(sound);

        self.sounds_that_maybe_happened
            .values()
            .map(|x| x.iter())
            .flatten()
            .find(|x| **x == sound)
            .is_none()
    }

    pub fn pop_sounds_since(&mut self, from: usize, to: usize) {
        //println!("popping sounds from {from} to {to}");

        match std::mem::replace(&mut self.current_rollback, Some(from)) {
            Some(x) => println!("should have crashed sound 45"),
            None => (),
        };

        for a in from..=to {
            if let Some(old_sounds) = self.sounds_that_did_happen.remove(&a) {
                self.sounds_that_maybe_happened.insert(a, old_sounds);
            }
        }
    }

    pub fn delete_non_matched(&mut self) {
        let old_sounds = std::mem::replace(&mut self.sounds_that_maybe_happened, HashMap::new());
        let fc = unsafe { *SOKU_FRAMECOUNT };
        let old = match self.current_rollback.take() {
            Some(x) => x,
            None => {
                println!("should have crashed here, sound 65");
                return;
                //panic!();
            }
        };

        let new_sounds: HashSet<usize> = (old..=fc)
            .flat_map(|x| self.sounds_that_did_happen.remove(&x))
            .map(|x| x.into_iter())
            .flatten()
            .collect();

        for (frame, sound) in old_sounds
            .into_iter()
            .map(|(frame, sounds)| sounds.into_iter().map(move |x| (frame, x)))
            .flatten()
        {
            //to do: not only delete sounds, but also restart them/shift them

            if !new_sounds.contains(&sound) {
                let played_recently = (old.saturating_sub(4)..old)
                    .filter_map(|x| self.sounds_that_did_happen.get(&x))
                    .map(|x| x.iter())
                    .flatten()
                    .find(|x| **x == sound)
                    .is_some();

                if !played_recently {
                    force_sound_skip(sound);

                    /*println!(
                        "sound {}, from frame {} deleted at frame {}",
                        sound,
                        frame,
                        unsafe { *SOKU_FRAMECOUNT },
                    ); */
                } else {
                    //println!("sound {} would have been skipped but wasnt", sound)
                }
            } else {
                self.sounds_that_did_happen
                    .entry(frame)
                    .or_insert_with(|| Vec::new())
                    .push(sound)
                //self.insert_sound(*frame, *sound);
                //println!("sound retained");
            }
        }
    }
}



================================================
File: .github/workflows/build.yml
================================================
name: build
on: workflow_dispatch

jobs:
  build:
    name: build
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4
      - uses: dtolnay/rust-toolchain@stable
        with:
          toolchain: nightly-2024-02-03
          targets: i686-pc-windows-msvc
      - run: cargo build --target i686-pc-windows-msvc --release --config profile.release.debug=true --config 'profile.release.split-debuginfo="packed"' --config 'profile.release.strip="none"'
      - run: |
          cp giuroll.ini target/i686-pc-windows-msvc/release/
          cp LICENSE target/i686-pc-windows-msvc/release/LICENSE.txt
          echo [InternetShortcut] > source-code-and-the-build.url
          # https://stackoverflow.com/a/70566764
          echo URL=${{ github.server_url }}/${{ github.repository }}/actions/runs/${{ github.run_id }} >> source-code-and-the-build.url
          cp source-code-and-the-build.url target/i686-pc-windows-msvc/release/
      - uses: actions/upload-artifact@v4
        with:
          name: Giuroll binary
          path: |
            target/i686-pc-windows-msvc/release/giuroll.dll
            target/i686-pc-windows-msvc/release/giuroll.ini
            target/i686-pc-windows-msvc/release/LICENSE.txt
            target/i686-pc-windows-msvc/release/source-code-and-the-build.url
      - uses: actions/upload-artifact@v4
        with:
          name: Giuroll symbols (PDB)
          path: |
            target/i686-pc-windows-msvc/release/giuroll.pdb


