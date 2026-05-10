fn main() {
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

    println!("cargo:rustc-link-lib=setupapi");
}
