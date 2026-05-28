add_executable(
    scsi_ua_probe
    main.c
)

target_compile_definitions(
    scsi_ua_probe
    PRIVATE
    UNICODE
    _UNICODE
)

if (MINGW)
    target_compile_options(
        scsi_ua_probe
        PRIVATE
        -municode
    )
    target_link_options(
        scsi_ua_probe
        PRIVATE
        -municode
        -static
        -static-libgcc
    )
endif()
