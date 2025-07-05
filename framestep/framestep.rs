use process_memory::*; // windows api is pain i want to use wrappers whenever possible
use sdl2::controller::{Button, GameController};
use sdl2::event::Event;
use sdl2::GameControllerSubsystem;
use std::{env, ffi, fs, mem};
use windows::core::*;
use windows::Win32::Foundation::*;
use windows::Win32::System::Diagnostics::Debug::*;
use windows::Win32::System::Threading::*;

extern crate sdl2;

const PAUSE_BUTTON: Button = Button::Back;
const CONTINUE_BUTTON: Button = Button::A;

// fn any_controller_button(gc: GameControllerSubsystem, b: Button) -> bool {
//     gc.update();
//     for joystick_idx in 0..gc.num_joysticks().unwrap() {
//         println!("{joystick_idx}");
//         let joystick = gc.open(joystick_idx).unwrap();
//         if joystick.button(b) {
//             return true;
//         }
//     }
//     false
// }

fn main() {
    sdl2::hint::set("SDL_JOYSTICK_THREAD", "1");
    sdl2::hint::set("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    let sdl_context = sdl2::init().unwrap();
    let game_controller_subsystem = sdl_context.game_controller().unwrap();
    let event_subsystem = sdl_context.event().unwrap();
    let mut event_pump = sdl_context.event_pump().unwrap();

    // do not actually care but need to store them for events to come through
    let mut controllers: Vec<GameController> = Vec::new(); 
                                                           
                                                           
    // for event in event_pump.wait_iter() {
    //     println!("{:?}", event);
    //     match event {
    //         Event::ControllerDeviceAdded {
    //             timestamp: _,
    //             which,
    //         } => {controllers.push(game_controller_subsystem.open(which).unwrap());},
    //         _ => (),
    //     }
    // }

    let dircontents = fs::read_dir(env::current_dir().unwrap()).unwrap();
    let paths = dircontents.map(|x| x.unwrap().path());
    let mut exepath = None;
    for path in paths {
        if path.extension() == Some(ffi::OsStr::new("kgt"))
            && fs::exists(path.with_extension("exe")).unwrap()
        {
            assert!(
                exepath.is_none(),
                "Error: Multiple potential game executables found."
            );
            exepath = Some(path.with_extension("exe"));
            println!("{:?}", exepath);
        }
    }
    assert!(exepath.is_some(), "Error: No game executables found.");

    let mut si: STARTUPINFOW = unsafe { mem::zeroed() };
    let mut pi: PROCESS_INFORMATION = unsafe { mem::zeroed() };
    unsafe {
        let _ = CreateProcessW(
            &HSTRING::from(exepath.unwrap().as_os_str()),
            None,
            None,                    // lpProcessAttributes
            None,                    // lpThreadAttributes
            false,                   // bInheritHandles
            DEBUG_ONLY_THIS_PROCESS, // dwCreationFlags
            None,                    // lpEnvironment
            None,                    // lpCurrentDirectory
            &mut si,                 // lpStartupInfo
            &mut pi,                 // lpProcessInformation
        );
    }

    let handle = (pi.dwProcessId as Pid).try_into_process_handle().unwrap();
    let mut debug_event: DEBUG_EVENT = unsafe { mem::zeroed() };
    let mut context: WOW64_CONTEXT = unsafe { mem::zeroed() };
    let mut paused: bool = false;
    let mut pause_button_released = true;
    loop {
        unsafe {
            let _ = WaitForDebugEvent(&mut debug_event, INFINITE);
        }
        match debug_event.dwDebugEventCode {
            // case     directly before call function to set inputs
            // before   00405be4:   8d 73 01            LEA ESI, [EBX+0x1];
            // after    00405be4:   cc 90 90            INT3; NOP; NOP;
            // here     esi = ebx + 1;

            // case     directly at the start input setting function, before it gets keyboard state
            // before   004146d0:   53                  PUSH EBX;
            // after    004146d0:   cc                  INT3;
            // here     esp -= 4; write ebx to address in esp (4 bytes)

            // case     near end of set input function, while cleaning up stack and registers
            // before   00414837:   89 1d 04 fa 4c 00   MOV [004cfa04], EBX;
            // after    00414837:   cc 90 90 90 90 90   INT3; NOP; NOP; NOP; NOP; NOP;
            // here     write ebx to address 004cfa04 (4 bytes)
            CREATE_PROCESS_DEBUG_EVENT => {
                println!("create process");
                // let _ = handle.put_address(0x00405be4, &[0xcc, 0x90, 0x90]);
                let _ = handle.put_address(0x004146d0, &[0xcc]); // replace PUSH EBX; with INT3;
            }
            EXCEPTION_DEBUG_EVENT => {
                // println!("exception");
                match unsafe { debug_event.u.Exception.ExceptionRecord.ExceptionAddress as usize } {
                    // 0x00405be4 => {
                    0x004146d0 => {
                        context.ContextFlags = WOW64_CONTEXT_FULL;
                        unsafe {
                            let _ = Wow64GetThreadContext(pi.hThread, &mut context);
                        }
                        // context.Esi = context.Ebx + 1;
                        context.Esp -= 4;
                        let _ = handle.put_address(context.Esp as usize, &context.Edx.to_le_bytes());
                        unsafe {
                            let _ = Wow64SetThreadContext(pi.hThread, &mut context);
                        }
                        if !paused {
                            // println!("in unpaused");
                            for event in event_pump.poll_iter() {
                                println!("{:?}", event);
                                match event {
                                    Event::ControllerButtonUp {
                                        button: PAUSE_BUTTON,
                                        ..
                                    } => {
                                        pause_button_released = true;
                                    }
                                    Event::ControllerButtonDown {
                                        button: PAUSE_BUTTON,
                                        ..
                                    } if pause_button_released => {
                                        paused = true;
                                        pause_button_released = false;
                                        break;
                                    }
                                    Event::ControllerDeviceAdded {
                                        which,
                                        ..
                                    } => {
                                        // todo: swap to insert and index and add removing
                                        controllers.push(game_controller_subsystem.open(which).unwrap());
                                    }
                                    _ => (),
                                }
                            }
                        }
                        if paused {
                            println!("in paused");
                            loop{
                                let event = event_pump.wait_event();
                                println!("{:?}", event);
                                match event {
                                    Event::ControllerButtonUp {
                                        button: PAUSE_BUTTON,
                                        ..
                                    } => {
                                        pause_button_released = true;
                                    }
                                    Event::ControllerButtonDown {
                                        button: PAUSE_BUTTON,
                                        ..
                                    } if pause_button_released => {
                                        pause_button_released = false;
                                        break;
                                    }
                                    Event::ControllerButtonDown {
                                        button: CONTINUE_BUTTON,
                                        ..
                                    } if !pause_button_released   => {
                                        paused = false;
                                        break;
                                    }
                                    Event::ControllerDeviceAdded {
                                        which,
                                        ..
                                    } => {
                                        // todo: swap to insert and index and add removing
                                        controllers.push(game_controller_subsystem.open(which).unwrap());
                                    }
                                    _ => (),
                                }
                            }
                        }
                        // println!("{:?}", context.Esi);
                        // println!("{:?}", context.Ebx);
                    }
                    _ => (),
                }
                //unsafe {
                //    println!("{:?}", debug_event.u.Exception);
                //}
            }
            _ => (),
        }
        unsafe {
            let _ = ContinueDebugEvent(
                debug_event.dwProcessId,
                debug_event.dwThreadId,
                DBG_EXCEPTION_HANDLED, // theoretically should be setting this based on the actual
                                       // exception but just going to assume it is not important
                                       // for now :)
            );
        }
        if debug_event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT {
            break;
        }
    }
}
