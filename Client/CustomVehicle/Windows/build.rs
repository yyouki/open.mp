use std::{env, fs, path::PathBuf};

fn read_u16(bytes: &[u8], offset: usize) -> Option<u16> {
    let value = bytes.get(offset..offset.checked_add(2)?)?;
    Some(u16::from_le_bytes([value[0], value[1]]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    let value = bytes.get(offset..offset.checked_add(4)?)?;
    Some(u32::from_le_bytes([value[0], value[1], value[2], value[3]]))
}

fn is_i686_windows_dll(bytes: &[u8]) -> bool {
    if bytes.get(0..2) != Some(b"MZ") {
        return false;
    }

    let Some(pe_offset) = read_u32(bytes, 0x3C).and_then(|value| usize::try_from(value).ok())
    else {
        return false;
    };
    if bytes.get(pe_offset..pe_offset.saturating_add(4)) != Some(b"PE\0\0") {
        return false;
    }

    let Some(machine) = read_u16(bytes, pe_offset.saturating_add(4)) else {
        return false;
    };
    let Some(characteristics) = read_u16(bytes, pe_offset.saturating_add(22)) else {
        return false;
    };
    let Some(optional_header_magic) = read_u16(bytes, pe_offset.saturating_add(24)) else {
        return false;
    };

    // IMAGE_FILE_MACHINE_I386, IMAGE_FILE_DLL, and PE32 (not PE32+).
    machine == 0x014C && characteristics & 0x2000 != 0 && optional_header_magic == 0x010B
}

fn main() {
    let target = env::var("TARGET").unwrap_or_default();
    if target != "i686-pc-windows-msvc" {
        panic!("custom-vehicle-windows supports only i686-pc-windows-msvc targets");
    }

    let lib_dir = env::var("CUSTOM_VEHICLE_CORE_LIB_DIR")
        .expect("CUSTOM_VEHICLE_CORE_LIB_DIR must point to the CMake Release output directory");
    println!("cargo:rustc-link-search=native={lib_dir}");
    println!("cargo:rustc-link-lib=static=custom_vehicle_protocol");
    println!("cargo:rerun-if-env-changed=CUSTOM_VEHICLE_CORE_LIB_DIR");

    let host_path = env::var_os("CUSTOM_VEHICLE_HOST_DLL_PATH")
        .map(PathBuf::from)
        .expect("CUSTOM_VEHICLE_HOST_DLL_PATH must point to the pinned samp_client_sdk.dll");
    let host_path = host_path
        .canonicalize()
        .expect("CUSTOM_VEHICLE_HOST_DLL_PATH does not exist or cannot be resolved");
    let host_bytes = fs::read(&host_path)
        .expect("CUSTOM_VEHICLE_HOST_DLL_PATH could not be read");
    if !is_i686_windows_dll(&host_bytes) {
        panic!(
            "the embedded SDK host must be a valid 32-bit x86 Windows DLL: {}",
            host_path.display()
        );
    }

    println!(
        "cargo:rustc-env=CUSTOM_VEHICLE_HOST_DLL_PATH={}",
        host_path.display()
    );
    println!("cargo:rerun-if-changed={}", host_path.display());
    println!("cargo:rerun-if-env-changed=CUSTOM_VEHICLE_HOST_DLL_PATH");
}
