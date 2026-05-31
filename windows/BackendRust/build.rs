fn main() {
    for path in [
        "../AppKernel/include/appkernel.h",
        "../AppKernel/src/appkernel.c",
        "../AppKernel/src/common/ak_memory.c",
        "../AppKernel/src/common/ak_status.c",
        "../AppKernel/src/disk/ak_disk.c",
        "../AppKernel/src/event/ak_event.c",
        "../AppKernel/src/protocol/ak_protocol.c",
        "../AppKernel/src/session/ak_session.c",
        "../shared/yumedisk_proto.h",
    ] {
        println!("cargo:rerun-if-changed={}", path);
    }

    cc::Build::new()
        .include("../AppKernel/include")
        .include("../AppKernel/src")
        .include("../shared")
        .define("AK_BUILD_DLL", None)
        .define("WIN32_LEAN_AND_MEAN", None)
        .define("NOMINMAX", None)
        .files([
            "../AppKernel/src/appkernel.c",
            "../AppKernel/src/common/ak_memory.c",
            "../AppKernel/src/common/ak_status.c",
            "../AppKernel/src/disk/ak_disk.c",
            "../AppKernel/src/event/ak_event.c",
            "../AppKernel/src/protocol/ak_protocol.c",
            "../AppKernel/src/session/ak_session.c",
        ])
        .compile("appkernel");

    println!(
        "cargo:rustc-link-search=native={}",
        std::env::var("OUT_DIR").expect("OUT_DIR should exist")
    );
    println!("cargo:rustc-link-lib=static=appkernel");
    println!("cargo:rustc-link-lib=setupapi");
}
