use std::env;

fn main() {
    let target = env::var("TARGET").unwrap_or_default();
    if target != "i686-pc-windows-msvc" {
        panic!("custom-vehicle-windows supports only i686-pc-windows-msvc");
    }

    let lib_dir = env::var("CUSTOM_VEHICLE_CORE_LIB_DIR")
        .expect("CUSTOM_VEHICLE_CORE_LIB_DIR must point to the CMake Release output directory");

    println!("cargo:rustc-link-search=native={lib_dir}");
    println!("cargo:rustc-link-lib=static=custom_vehicle_protocol");
    println!("cargo:rerun-if-env-changed=CUSTOM_VEHICLE_CORE_LIB_DIR");
}
